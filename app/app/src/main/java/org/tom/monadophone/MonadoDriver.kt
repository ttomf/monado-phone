package org.tom.monadophone

import android.content.Context
import android.media.MediaCodec
import android.media.MediaFormat
import android.opengl.GLSurfaceView
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.Surface
import android.widget.Toast
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
import kotlin.time.Duration.Companion.milliseconds

private const val MCAST_ADDR = "239.1.1.1"
private const val PORT = 5500
private const val CONFIG_PORT = 5501
private const val STREAM_PORT = 5502
private const val POSE_PORT = 5503
private const val DISCOVER_MSG_PHONE = "MONADO_PHONE_DISCOVER_PHONE"
private const val DISCOVER_MSG_PC = "MONADO_PHONE_DISCOVER_PC"
private const val TAG = "MonadoDriver"

/**
 * Orchestrates the phone side of the Monado Phone HMD driver: discovery,
 * video stream reception and pose sending. Mirrors the wire protocol defined
 * in the Monado phone driver (phone_internals.h).
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
    private var configSocket: Socket? = null

    private var glSurfaceView: GLSurfaceView? = null
    private var poseSource: ArCorePose? = null
    private var shouldResume = false
    private var scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private lateinit var runtimeAddr: InetAddress

    fun setGlSurfaceView(view: GLSurfaceView) {
        if (glSurfaceView !== view) {
            poseSource?.close()
            poseSource = null
            glSurfaceView = view
            poseSource = ArCorePose(context, view)
            if (shouldResume) {
                poseSource?.resume()
            }
        }
    }

    fun resume() {
        shouldResume = true
        if (poseSource == null) {
            val view = glSurfaceView ?: return
            poseSource = ArCorePose(context, view)
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

        job = scope.launch {
            val discoverySocket = MulticastSocket()
            discoverySocket.timeToLive = 1
            discoverySocket.soTimeout = 500
            val msg = DISCOVER_MSG_PHONE.toByteArray(Charsets.UTF_8)
            val packet = DatagramPacket(msg, msg.size, InetAddress.getByName(MCAST_ADDR), PORT)

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
                        if (String(recvPacket.data, 0, recvPacket.length) == DISCOVER_MSG_PC) {
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
            configSocket = Socket(runtimeAddr, CONFIG_PORT)
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
            if (configSocket?.isClosed != true) {
                throw e
            }
        }
    }

    /**
     * Sends the latest ARCore pose to the PC over UDP.
     * The packet layout is defined in [PosePacket].
     */
    private suspend fun sendPose() {
        val src = poseSource
        if (src == null) {
            Log.w(TAG, "sendPose: no pose source")
            return
        }
        poseSocket = withContext(Dispatchers.IO) {
            DatagramSocket()
        }
        try {
            poseSocket?.use { s ->
                Log.d(TAG, "sending pose to ${runtimeAddr.hostAddress}:$POSE_PORT")
                var sent = 0L
                var lastLog = 0L
                while (currentCoroutineContext().isActive) {
                    val pose = src.latestPose()
                    if (pose == null) {
                        delay(10.milliseconds)
                        continue
                    }
                    val data = PosePacket.encode(pose)
                    val datagram = DatagramPacket(data, data.size, runtimeAddr, POSE_PORT)
                    s.send(datagram)
                    sent++
                    val now = System.nanoTime()
                    if (now - lastLog > 1_000_000_000L) {
                        Log.d(
                            TAG,
                            "pose sent: $sent, ts=${pose.timestampNs}, state=${pose.trackingState}"
                        )
                        lastLog = now
                    }
                    delay(5.milliseconds)
                }
            }
        } catch (e: Exception) {
            Log.e(TAG, "sendPose failed", e)
        } finally {
            poseSocket = null
        }
    }

    private suspend fun receiveVideo(surf: Surface) {
        val codec = try {
            MediaCodec.createDecoderByType("video/hevc").apply {
			configure(MediaFormat.createVideoFormat("video/hevc", 1920, 1080).apply {
                    setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
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
                    bind(InetSocketAddress(STREAM_PORT))
                }
            }
            streamSocket = socket
            socket.use { socket ->
                Log.d(TAG, "listening for stream on port $STREAM_PORT")
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
        configSocket?.close()
        configSocket = null
        job = null
    }

    fun destroy() {
        Log.d(TAG, "destroy")
        stop()
        poseSource?.pause()
        surface = null
    }

    fun restart() {
        toast("Reloading...")
        Log.d(TAG, "restart")
        stop()
        val s = surface
        if (s != null && s.isValid) {
            start(s)
        } else {
            Log.w(TAG, "restart: no valid surface")
        }
    }
}
