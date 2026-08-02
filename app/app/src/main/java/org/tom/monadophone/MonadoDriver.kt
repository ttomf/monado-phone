package org.tom.monadophone

import android.media.MediaCodec
import android.media.MediaFormat
import android.util.Log
import android.view.Surface
import java.io.ByteArrayOutputStream
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.MulticastSocket
import java.net.SocketTimeoutException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlin.time.Duration.Companion.milliseconds

private const val MCAST_ADDR = "239.1.1.1"
private const val PORT = 5500
private const val DISCOVER_MSG_PHONE = "MONADO_PHONE_DISCOVER_PHONE"
private const val DISCOVER_MSG_PC = "MONADO_PHONE_DISCOVER_PC"

object MonadoDriver {
    private var surface: Surface? = null
    private var job: Job? = null
    private var streamSocket: DatagramSocket? = null
    private var scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private lateinit var runtimeAddr: InetAddress

    fun start(surf: Surface) {
        Log.d("MonadoDriver", "start")
        if (!surf.isValid) {
            Log.w("MonadoDriver", "start: surface is not valid, ignoring")
            return
        }
        surface = surf

        job?.cancel() // Replace any previous job.
        streamSocket?.close() // Free the stream port synchronously.
        streamSocket = null

        job = scope.launch stream@{
            val discoverySocket = MulticastSocket()
            discoverySocket.timeToLive = 1
            discoverySocket.soTimeout = 500
            val msg = DISCOVER_MSG_PHONE.toByteArray(Charsets.UTF_8)
            val packet =
                DatagramPacket(msg, msg.size, InetAddress.getByName(MCAST_ADDR), PORT)

            Log.d("MonadoDriver", "start sending beacon packets")
            discoverySocket.use { socket ->
                // Periodically send beacon packets
                while (isActive) {
                    try {
                        socket.send(packet)
                    } catch (e: Exception) {
                        e.printStackTrace()
                    }
                    val buf = ByteArray(256)
                    val recvPacket = DatagramPacket(buf, buf.size)
                    try {
                        socket.receive(recvPacket)
                        Log.i("MonadoDriver", "beacon: received ${recvPacket.length} bytes")
                        if (String(recvPacket.data, 0, recvPacket.length) == DISCOVER_MSG_PC) {
                            // When paired, stop sending beacon packets
                            runtimeAddr = recvPacket.address
                            break
                        }
                    } catch (e: SocketTimeoutException) {
                    }
                    delay(500.milliseconds)
                }
                if (!this@stream.isActive) return@stream
                Log.d("MonadoDriver", "paired with ${runtimeAddr.hostAddress}, opening stream")
                receiveVideo(surf)
            }
        }
    }

    private suspend fun receiveVideo(surf: Surface) {
        val codec = try {
            MediaCodec.createDecoderByType("video/hevc").apply {
                configure(MediaFormat.createVideoFormat("video/hevc", 1280, 720), surf, null, 0)
                start()
            }
        } catch (e: Exception) {
            Log.e("MonadoDriver", "failed to create/configure codec", e)
            return
        }
        Log.d("MonadoDriver", "codec started")

        val startCode = byteArrayOf(0, 0, 0, 1)
        val buf = ByteArray(65536)
        val packet = DatagramPacket(buf, buf.size)
        val fu = ByteArrayOutputStream()

        try {
            val socket = DatagramSocket(null).apply {
                reuseAddress = true
                soTimeout = 500
                // Large kernel receive buffer so bursty RTP traffic does not get dropped.
                setReceiveBufferSize(4 * 1024 * 1024)
                bind(InetSocketAddress(PORT))
            }
            streamSocket = socket
            socket.use { socket ->
                Log.d("MonadoDriver", "listening for stream on port $PORT")
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

                    // Track RTP sequence gaps (packet loss) for diagnostics.
                    val seq = ((buf[2].toInt() and 0xFF) shl 8) or (buf[3].toInt() and 0xFF)
                    if (lastSeq >= 0 && seq != ((lastSeq + 1) and 0xFFFF)) {
                        val gap = if (seq > lastSeq) seq - lastSeq - 1 else (seq + 0x10000 - lastSeq - 1)
                        lostPackets += gap
                        val now = System.nanoTime()
                        if (now - lastLossLog > 1_000_000_000L) {
                            Log.w("MonadoDriver", "RTP gap: last=$lastSeq got=$seq, lost≈$lostPackets")
                            lastLossLog = now
                        }
                    }
                    lastSeq = seq

                    // RTP header, skip CSRCs if present.
                    val csrcCount = buf[0].toInt() and 0x0F
                    val hdrLen = 12 + csrcCount * 4
                    if (n <= hdrLen) continue

                    val payload = buf.copyOfRange(hdrLen, n)
                    if (payload.size < 2) continue

                    // HEVC 2-byte NAL header, the type is in the first byte.
                    val nalType = (payload[0].toInt() shr 1) and 0x3F

                    val ok = when {
                        // Single NAL unit packet.
                        nalType <= 47 -> {
                            val nal = ByteArray(4 + payload.size)
                            startCode.copyInto(nal)
                            payload.copyInto(nal, 4)
                            feedCodec(codec, nal, nalType)
                        }

                        // Aggregation packet.
                        nalType == 48 -> {
                            var off = 2
                            var allOk = true
                            while (off + 2 < payload.size) {
                                val size =
                                    ((payload[off].toInt() and 0xFF) shl 8) or
                                        (payload[off + 1].toInt() and 0xFF)
                                if (off + 2 + size > payload.size) break
                                val sub = payload.copyOfRange(off + 2, off + 2 + size)
                                if (sub.size < 2) break
                                val t = (sub[0].toInt() shr 1) and 0x3F
                                val nal = ByteArray(4 + sub.size)
                                startCode.copyInto(nal)
                                sub.copyInto(nal, 4)
                                if (!feedCodec(codec, nal, t)) {
                                    allOk = false
                                    break
                                }
                                off += 2 + size
                            }
                            allOk
                        }

                        // Fragmentation unit.
                        nalType == 49 -> {
                            if (payload.size < 3) {
                                true
                            } else {
                                val fuHeader = payload[2].toInt()
                                val start = (fuHeader and 0x80) != 0
                                val end = (fuHeader and 0x40) != 0
                                val origType = fuHeader and 0x3F

                                if (start) {
                                    fu.reset()
                                    fu.write(startCode)
                                    fu.write((payload[0].toInt() and 0x81) or (origType shl 1))
                                    fu.write(payload[1].toInt())
                                    fu.write(payload, 3, payload.size - 3)
                                } else if (fu.size() > 0) {
                                    fu.write(payload, 3, payload.size - 3)
                                }

                                if (end && fu.size() > 0) {
                                    val okFeed = feedCodec(codec, fu.toByteArray(), origType)
                                    fu.reset()
                                    okFeed
                                } else {
                                    true
                                }
                            }
                        }

                        else -> true
                    }
                    // A single decode hiccup must not kill the whole stream.
                    if (!ok) {
                        decodeErrors++
                        val now = System.nanoTime()
                        if (now - lastDecodeErrorLog > 1_000_000_000L) {
                            Log.w("MonadoDriver", "feedCodec failed $decodeErrors times, continuing")
                            lastDecodeErrorLog = now
                        }
                    }
                }
            }
        } catch (e: Exception) {
            Log.e("MonadoDriver", "stream error", e)
        } finally {
            try {
                codec.stop()
            } catch (e: Exception) {
            }
            codec.release()
            Log.d("MonadoDriver", "codec released")
        }
    }

    /**
     * Feeds one complete NAL unit (with start code) to the decoder.
     * VPS/SPS/PPS (types 32-34) are sent in-band by the sender with every
     * packet, so they are queued with plain flags; BUFFER_FLAG_CODEC_CONFIG
     * on input is ignored by most codecs and can stall Qualcomm decoders.
     */
    private fun feedCodec(codec: MediaCodec, nal: ByteArray, nalType: Int): Boolean {
        return try {
            val index = codec.dequeueInputBuffer(10000)
            if (index < 0) {
                Log.w("MonadoDriver", "feedCodec: no input buffer within 10s")
                return false
            }

            val input = codec.getInputBuffer(index)
            if (input == null) {
                Log.w("MonadoDriver", "feedCodec: getInputBuffer($index) is null")
                return false
            }
            input.clear()
            input.put(nal)

            codec.queueInputBuffer(index, 0, nal.size, System.nanoTime() / 1000, 0)

            // Drain output buffers onto the surface.
            val info = MediaCodec.BufferInfo()
            while (true) {
                val out = codec.dequeueOutputBuffer(info, 0)
                when {
                    out == MediaCodec.INFO_TRY_AGAIN_LATER -> break
                    out == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                        Log.d("MonadoDriver", "output format: ${codec.outputFormat}")
                    }

                    out >= 0 -> codec.releaseOutputBuffer(out, true)
                    else -> break
                }
            }
            true
        } catch (e: Exception) {
            Log.e("MonadoDriver", "feedCodec failed", e)
            false
        }
    }

    fun stop() {
        Log.d("MonadoDriver", "stop")
        job?.cancel()
        streamSocket?.close()
        streamSocket = null
        job = null
    }

    fun destroy() {
        Log.d("MonadoDriver", "destroy")
        stop()
        surface = null
    }

    fun restart() {
        Log.d("MonadoDriver", "restart")
        stop()
        val s = surface
        if (s != null && s.isValid) {
            start(s)
        } else {
            Log.w("MonadoDriver", "restart: no valid surface")
        }
    }
}
