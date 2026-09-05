package org.tom.monadophone

import android.content.Context
import android.media.MediaCodec
import android.media.MediaFormat
import android.opengl.GLSurfaceView
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.Surface
import android.widget.Toast
import com.google.common.math.Stats.meanOf
import com.google.mediapipe.tasks.vision.handlandmarker.HandLandmarkerResult
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.MulticastSocket
import java.net.Socket
import java.net.SocketException
import java.net.SocketTimeoutException
import kotlin.math.pow
import kotlin.math.sqrt
import kotlin.time.Duration.Companion.milliseconds

private const val DISCOVER_MSG_PHONE = "MONADO_PHONE_DISCOVER_PHONE"
private const val DISCOVER_MSG_PC = "MONADO_PHONE_DISCOVER_PC"
private const val TAG = "MonadoDriver"

/**
 * Manages the phone side of the Monado Phone HMD driver: discovery,
 * video stream reception and pose sending. Mirrors the wire protocol defined
 * in the Monado phone driver.
 */
class MonadoDriver(
    private val context: Context
) {
    private var surface: Surface? = null
    private var job: Job? = null

    @Volatile
    private var streamSocket: DatagramSocket? = null

    @Volatile
    private var poseSocket: DatagramSocket? = null

    @Volatile
    private var handsSocket: DatagramSocket? = null

    @Volatile
    private var configSocket: Socket? = null

    private var glSurfaceView: GLSurfaceView? = null
    private var poseSource: ArCorePose? = null
    private var handTracker: HandTracker = HandTracker(context, ::processHandsResult)
    private var shouldResume = false
    private var scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private lateinit var runtimeAddr: InetAddress

    fun setGlSurfaceView(view: GLSurfaceView) {
        if (glSurfaceView !== view) {
            poseSource?.close()
            poseSource = null
            glSurfaceView = view
            poseSource = ArCorePose(context, view, handTracker)
            if (shouldResume) {
                poseSource?.resume()
            }
        }
    }

    fun resume() {
        shouldResume = true
        if (poseSource == null) {
            val view = glSurfaceView ?: return
            poseSource = ArCorePose(context, view, handTracker)
        }
        poseSource?.resume()
    }

    fun pause() {
        shouldResume = false
        poseSource?.pause()
    }

    fun start(surf: Surface) {
        Log.d(TAG, "start")
        if (!surf.isValid) {
            Log.w(TAG, "start: surface is not valid, ignoring")
            return
        }
        surface = surf

        job?.cancel() // Replace any previous job
        streamSocket?.close() // Free the stream port synchronously
        streamSocket = null
        poseSocket?.close()
        poseSocket = null
        handsSocket?.close()
        handsSocket = null
        configSocket?.close()
        configSocket = null

        job = scope.launch {
            val discoverySocket = MulticastSocket()
            discoverySocket.timeToLive = 1
            discoverySocket.soTimeout = 500
            val msg = DISCOVER_MSG_PHONE.toByteArray(Charsets.UTF_8)
            val packet = DatagramPacket(msg, msg.size, InetAddress.getByName(Settings.multicastAddr), Settings.port)

            Log.d(TAG, "start sending beacon packets")
            discoverySocket.use { socket ->
                // Periodically send beacon packets
                toast("Starting discovery...")
                var attempts = 0
                while (currentCoroutineContext().isActive) {
                    try {
                        socket.send(packet)
                    } catch (e: Exception) {
                        e.printStackTrace()
                    }
                    val buf = ByteArray(256)
                    val recvPacket = DatagramPacket(buf, buf.size)
                    try {
                        socket.receive(recvPacket)
                        Log.i(TAG, "beacon: received ${recvPacket.length} bytes")
                        if (String(
                                recvPacket.data,
                                0,
                                recvPacket.length,
                                Charsets.UTF_8
                            ) == DISCOVER_MSG_PC
                        ) {
                            // When paired, stop sending beacon packets
                            runtimeAddr = recvPacket.address
                            break
                        }
                    } catch (e: SocketTimeoutException) {
                    }
                    // Slow down after a while to save battery if the PC is not listening.
                    val interval = if (++attempts > 120) 5000L else 500L
                    delay(interval.milliseconds)
                }
            }
            if (currentCoroutineContext().isActive) {
                Log.d(TAG, "paired with ${runtimeAddr.hostAddress}, opening stream")
                toast("Connecting to ${runtimeAddr.hostAddress}")
                if (Settings.enableHandTracking) {
                    handTracker.setup()
                    handsSocket = DatagramSocket()
                }
                coroutineScope {
                    launch { receiveVideo(surf) }
                    launch { configHandler() }
                    launch { sendPose() }
                }
            }
        }
    }

    private fun toast(msg: String) {
        Handler(Looper.getMainLooper()).post {
            Toast.makeText(context, msg, Toast.LENGTH_SHORT).show()
        }
    }

    private fun configHandler() {
        try {
            configSocket = Socket(runtimeAddr, Settings.configPort)
            val input = configSocket!!.getInputStream()
            val buffer = ByteArray(4096)

            while (true) {
                val n = input.read(buffer)
                if (n == -1) {
                    toast("PC Disconnected")
                    restart()
                    break
                }
                when (buffer[0].toInt().toChar()) {
                    'r' -> restart()
                }
            }
        } catch (e: SocketException) {
            Log.d("MonadoDriver", "Config socket closed: ${e.message}")
        } catch (e: Exception) {
            Log.e("MonadoDriver", "Error in configHandler", e)
        } finally {
            toast("PC Disconnected")
            restart()
        }
    }

    /**
     * Sends the latest ARCore pose to the PC over UDP.
     * The packet layout is defined in [PosePacket].
     */
    private suspend fun sendPose() {
        if (poseSource == null) {
            Log.w(TAG, "sendPose: no pose source")
            return
        }
        poseSocket = withContext(Dispatchers.IO) {
            DatagramSocket()
        }
        try {
            Log.d(TAG, "sending pose to ${runtimeAddr.hostAddress}:${Settings.posePort}")
            while (currentCoroutineContext().isActive) {
                val pose = poseSource!!.latestPose()
                val offset = poseSource!!.offset()
                if (pose == null) {
                    delay(10.milliseconds)
                    continue
                }
                if (!Settings.enable6DOFTracking) {
                    pose.tx = 0f
                    pose.ty = 0f
                    pose.tz = 0f
                }
                val data = PosePacket.encode(pose - offset)
                val datagram = DatagramPacket(data, data.size, runtimeAddr, Settings.posePort)
                withContext(Dispatchers.IO) {
                    poseSocket?.send(datagram)
                }
                delay(5.milliseconds)
            }
        } catch (e: Exception) {
            Log.e(TAG, "sendPose failed", e)
        }
    }

    private fun processHandsResult(result: HandLandmarkerResult) {
        val worldLandmarks = result.worldLandmarks()
        val handedness = result.handedness()
        val localLandmarks = result.landmarks()

        val landmarks = HandLandmarks(result.timestampMs() * 1_000_000)

        val focalLength = HAND_WIDTH

        for ((handIdx, hand) in localLandmarks.withIndex()) {
            val world = worldLandmarks[handIdx]
            val label = handedness[handIdx][0].categoryName()

            // Mean distance between four fingers and wrist
            val mcpIndices = intArrayOf(5, 9, 13, 17)
            val dists = mutableListOf<Float>()
            for (mcp in mcpIndices) {
                val dx = (hand[0].x() - hand[mcp].x()) * HAND_WIDTH
                val dy = (hand[0].y() - hand[mcp].y()) * HAND_HEIGHT
                dists.add(sqrt(dx.pow(2) + dy.pow(2)))
            }
            val palmSizePx = meanOf(dists)

            // Average distance between finger and wrist is 0.09 m
            val depth = (focalLength * 0.09) / (palmSizePx + 1e-6) // Avoid zero-division

            if (label == "Left") {
                landmarks.leftHand = FloatArray(21 * 3)
            }
            if (label == "Right") {
                landmarks.rightHand = FloatArray(21 * 3)
            }

            for (i in hand.indices) {
                val z = depth + world[i].z()
                val x = (hand[i].x() * HAND_WIDTH - HAND_WIDTH / 2) * z / focalLength
                val y = (hand[i].y() * HAND_HEIGHT - HAND_HEIGHT / 2) * z / focalLength

                val offset = i * 3
                if (label == "Left") {
                    landmarks.leftHand!![offset] = x.toFloat()
                    landmarks.leftHand!![offset + 1] = -y.toFloat()
                    landmarks.leftHand!![offset + 2] = -z.toFloat()
                }
                if (label == "Right") {
                    landmarks.rightHand!![offset] = x.toFloat()
                    landmarks.rightHand!![offset + 1] = -y.toFloat()
                    landmarks.rightHand!![offset + 2] = -z.toFloat()
                }
            }
        }

        val data = HandsPacket.encode(landmarks)
        val datagram = DatagramPacket(data, data.size, runtimeAddr, Settings.handsPort)
        handsSocket?.send(datagram)
    }

    private suspend fun receiveVideo(surf: Surface) {
        val codec = try {
            MediaCodec.createDecoderByType("video/hevc").apply {
                configure(MediaFormat.createVideoFormat("video/hevc", 1920, 1080).apply {
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                        setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
                    }
                }, surf, null, 0)
                start()
            }
        } catch (e: Exception) {
            Log.e(TAG, "failed to create/configure codec", e)
            toast("Codec error")
            return
        }
        Log.d(TAG, "codec started")

        val buf = ByteArray(65536)
        val packet = DatagramPacket(buf, buf.size)
        val depacketizer = Depacketizer()

        try {
            val socket = withContext(Dispatchers.IO) {
                DatagramSocket(null).apply {
                    reuseAddress = true
                    soTimeout = 500
                    // Large kernel receive buffer so bursty RTP traffic does not get dropped
                    receiveBufferSize = 4 * 1024 * 1024
                    bind(InetSocketAddress(Settings.streamPort))
                }
            }
            streamSocket = socket
            socket.use { socket ->
                Log.d(TAG, "listening for stream on port ${Settings.streamPort}")
                var lastSeq = -1
                var lostPackets = 0L
                var lastLossLog = 0L
                var decodeErrors = 0L
                var lastDecodeErrorLog = 0L
                while (currentCoroutineContext().isActive) {
                    try {
                        socket.receive(packet)
                    } catch (e: SocketTimeoutException) {
                        continue
                    } catch (e: Exception) {
                        break
                    }

                    val n = packet.length
                    if (n < 12) continue

                    // Track RTP sequence gaps (packet loss) for diagnostics
                    val seq = ((buf[2].toInt() and 0xFF) shl 8) or (buf[3].toInt() and 0xFF)
                    if (lastSeq >= 0 && seq != ((lastSeq + 1) and 0xFFFF)) {
                        val gap =
                            if (seq > lastSeq) seq - lastSeq - 1 else (seq + 0x10000 - lastSeq - 1)
                        lostPackets += gap
                        val now = System.nanoTime()
                        if (now - lastLossLog > 1_000_000_000L) {
                            Log.w(TAG, "RTP gap: last=$lastSeq got=$seq, lost≈$lostPackets")
                            lastLossLog = now
                        }
                    }
                    lastSeq = seq

                    // RTP header, skip CSRCs if present
                    val csrcCount = buf[0].toInt() and 0x0F
                    val hdrLen = 12 + csrcCount * 4
                    if (n <= hdrLen) continue

                    val payload = buf.copyOfRange(hdrLen, n)
                    for (nal in depacketizer.depacketize(payload)) {
                        // A single decode hiccup must not kill the whole stream
                        if (!feedCodec(codec, nal)) {
                            decodeErrors++
                            val now = System.nanoTime()
                            if (now - lastDecodeErrorLog > 1_000_000_000L) {
                                Log.w(TAG, "feedCodec failed $decodeErrors times, continuing")
                                lastDecodeErrorLog = now
                            }
                        }
                    }
                }
            }
        } catch (e: Exception) {
            Log.e(TAG, "stream error", e)
            toast("Stream error")
        } finally {
            try {
                codec.stop()
            } catch (e: Exception) {
            }
            codec.release()
            Log.d(TAG, "codec released")
        }
    }

    /**
     * Feeds one complete NAL unit (with start code) to the decoder.
     * VPS/SPS/PPS (types 32-34) are sent in-band by the sender with every
     * packet, so they are queued with plain flags; BUFFER_FLAG_CODEC_CONFIG
     * on input is ignored by most codecs and can stall Qualcomm decoders.
     */
    private fun feedCodec(codec: MediaCodec, nal: ByteArray): Boolean {
        return try {
            val index = codec.dequeueInputBuffer(10_000) // 10 ms, never block the receive loop
            if (index < 0) {
                // No input buffer right now, drop this packet rather than
                // stalling the RTP receive loop.
                return true
            }

            val input = codec.getInputBuffer(index)
            if (input == null) {
                Log.w(TAG, "feedCodec: getInputBuffer($index) is null")
                return false
            }
            input.clear()
            input.put(nal)

            codec.queueInputBuffer(index, 0, nal.size, System.nanoTime() / 1000, 0)

            // Drain output buffers onto the surface
            val info = MediaCodec.BufferInfo()
            while (true) {
                val out = codec.dequeueOutputBuffer(info, 0)
                when {
                    out == MediaCodec.INFO_TRY_AGAIN_LATER -> break
                    out == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                        Log.d(TAG, "output format: ${codec.outputFormat}")
                    }

                    out >= 0 -> codec.releaseOutputBuffer(out, true)
                    else -> break
                }
            }
            true
        } catch (e: Exception) {
            Log.e(TAG, "feedCodec failed", e)
            false
        }
    }

    fun stop() {
        Log.d(TAG, "stop")
        job?.cancel()
        streamSocket?.close()
        streamSocket = null
        poseSocket?.close()
        poseSocket = null
        handsSocket?.close()
        handsSocket = null
        configSocket?.close()
        configSocket = null
        job = null
    }

    fun destroy() {
        Log.d(TAG, "destroy")
        stop()
        poseSource?.pause()
        handTracker.close()
        surface = null
    }

    fun restart() {
        toast("Reloading...")
        Log.d(TAG, "restart")
        stop()
        val s = surface
        if (s != null && s.isValid) {
            start(s)
            poseSource?.pause()
            poseSource?.resume()
        } else {
            Log.w(TAG, "restart: no valid surface")
        }
    }

    fun recenter() {
        poseSource?.recenter()
    }
}
