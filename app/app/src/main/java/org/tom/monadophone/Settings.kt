package org.tom.monadophone

import android.content.Context
import android.content.SharedPreferences
import androidx.compose.runtime.mutableStateOf
import androidx.core.content.edit

object Settings {
    private lateinit var prefs: SharedPreferences

    private val enableHandTrackingState = mutableStateOf(true)
    private val enable6DOFTrackingState = mutableStateOf(true)
    private val multicastAddrState = mutableStateOf("239.1.1.1")
    private val portState = mutableStateOf(5500)
    private val configPortState = mutableStateOf(5501)
    private val streamPortState = mutableStateOf(5502)
    private val posePortState = mutableStateOf(5503)
    private val handsPortState = mutableStateOf(5504)

    fun init(context: Context) {
        prefs = context.applicationContext
            .getSharedPreferences("settings", Context.MODE_PRIVATE)

        enableHandTrackingState.value = prefs.getBoolean("enable_hand_tracking", true)
        enable6DOFTrackingState.value = prefs.getBoolean("enable_6dof_tracking", true)
        multicastAddrState.value = prefs.getString("multicast_addr", "239.1.1.1") ?: "239.1.1.1"
        portState.value = prefs.getInt("port", 5500)
        configPortState.value = prefs.getInt("config_port", 5501)
        streamPortState.value = prefs.getInt("stream_port", 5502)
        posePortState.value = prefs.getInt("pose_port", 5503)
        handsPortState.value = prefs.getInt("hands_port", 5504)
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
        get() = portState.value
        set(value) {
            portState.value = value
            prefs.edit { putInt("port", value) }
        }

    var configPort: Int
        get() = configPortState.value
        set(value) {
            configPortState.value = value
            prefs.edit { putInt("config_port", value) }
        }

    var streamPort: Int
        get() = streamPortState.value
        set(value) {
            streamPortState.value = value
            prefs.edit { putInt("stream_port", value) }
        }

    var posePort: Int
        get() = posePortState.value
        set(value) {
            posePortState.value = value
            prefs.edit { putInt("pose_port", value) }
        }

    var handsPort: Int
        get() = handsPortState.value
        set(value) {
            handsPortState.value = value
            prefs.edit { putInt("hands_port", value) }
        }
}
