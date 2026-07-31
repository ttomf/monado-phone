package org.tom.monadophone

import android.util.Log
import android.view.Surface
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.MulticastSocket
import java.net.SocketTimeoutException
import kotlin.time.Duration.Companion.milliseconds

private const val MCAST_ADDR = "239.1.1.1"
private const val MCAST_PORT = 5500
private const val DISCOVER_MSG_PHONE = "MONADO_PHONE_DISCOVER_PHONE"
private const val DISCOVER_MSG_PC = "MONADO_PHONE_DISCOVER_PC"

class MonadoDriver {
    private lateinit var surface: Surface
    private var job: Job? = null
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private lateinit var runtimeAddr: InetAddress

    fun start(surf: Surface) {
        Log.d("MonadoDriver", "start")
        surface = surf

        if (job?.isActive == true) return // Do not run another job if one is running

        job = scope.launch {
            val discoverySocket = MulticastSocket()
            discoverySocket.timeToLive = 1
            discoverySocket.soTimeout = 500
            val msg = DISCOVER_MSG_PHONE.toByteArray(Charsets.UTF_8)
            val packet =
                DatagramPacket(msg, msg.size, InetAddress.getByName(MCAST_ADDR), MCAST_PORT)

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
                val recvSocket = DatagramSocket()
                Log.d("MonadoDriver", "open receiving socket")
                restart()
            }
        }
    }

    fun stop() {
        Log.d("MonadoDriver", "stop")
        job?.cancel()
        job = null
    }

    fun destroy() {
        Log.d("MonadoDriver", "destroy")
        scope.cancel()
    }

    fun restart() {
        Log.d("MonadoDriver", "restart")
        stop()
        start(surface)
    }
}
