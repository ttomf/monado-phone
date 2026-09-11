package org.tom.monadophone

import android.content.Context
import android.content.SharedPreferences
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.core.content.edit

object Settings {
    private lateinit var prefs: SharedPreferences

    private val enableHandTrackingState = mutableStateOf(true)
    private val enable6DOFTrackingState = mutableStateOf(true)
    private val multicastAddrState = mutableStateOf("239.1.1.1")
    private val portState = mutableIntStateOf(5500)
    private val configPortState = mutableIntStateOf(5501)
    private val streamPortState = mutableIntStateOf(5502)
    private val posePortState = mutableIntStateOf(5503)
    private val handsPortState = mutableIntStateOf(5504)
    private val streamWState = mutableIntStateOf(1280)
    private val streamHState = mutableIntStateOf(720)

    fun init(context: Context) {
        prefs = context.applicationContext
            .getSharedPreferences("settings", Context.MODE_PRIVATE)

        enableHandTrackingState.value = prefs.getBoolean("enable_hand_tracking", true)
        enable6DOFTrackingState.value = prefs.getBoolean("enable_6dof_tracking", true)
        multicastAddrState.value = prefs.getString("multicast_addr", "239.1.1.1") ?: "239.1.1.1"
        portState.intValue = prefs.getInt("port", 5500)
        configPortState.intValue = prefs.getInt("config_port", 5501)
        streamPortState.intValue = prefs.getInt("stream_port", 5502)
        posePortState.intValue = prefs.getInt("pose_port", 5503)
        handsPortState.intValue = prefs.getInt("hands_port", 5504)
        streamWState.intValue = prefs.getInt("stream_w", 1280)
        streamHState.intValue = prefs.getInt("stream_h", 720)
    }

    var enableHandTracking: Boolean
        get() = enableHandTrackingState.value
        set(value) {
            enableHandTrackingState.value = value
            prefs.edit { putBoolean("enable_hand_tracking", value) }
        }

    var enable6DOFTracking: Boolean
        get() = enable6DOFTrackingState.value
        set(value) {
            enable6DOFTrackingState.value = value
            prefs.edit { putBoolean("enable_6dof_tracking", value) }
        }

    var multicastAddr: String
        get() = multicastAddrState.value
        set(value) {
            multicastAddrState.value = value
            prefs.edit { putString("multicast_addr", value) }
        }

    var port: Int
        get() = portState.intValue
        set(value) {
            portState.intValue = value
            prefs.edit { putInt("port", value) }
        }

    var configPort: Int
        get() = configPortState.intValue
        set(value) {
            configPortState.intValue = value
            prefs.edit { putInt("config_port", value) }
        }

    var streamPort: Int
        get() = streamPortState.intValue
        set(value) {
            streamPortState.intValue = value
            prefs.edit { putInt("stream_port", value) }
        }

    var posePort: Int
        get() = posePortState.intValue
        set(value) {
            posePortState.intValue = value
            prefs.edit { putInt("pose_port", value) }
        }

    var handsPort: Int
        get() = handsPortState.intValue
        set(value) {
            handsPortState.intValue = value
            prefs.edit { putInt("hands_port", value) }
        }

    var streamW: Int
        get() = streamWState.intValue
        set(value) {
            streamWState.intValue = value
            prefs.edit { putInt("stream_w", value) }
        }

    var streamH: Int
        get() = streamHState.intValue
        set(value) {
            streamHState.intValue = value
            prefs.edit { putInt("stream_h", value) }
        }
}
