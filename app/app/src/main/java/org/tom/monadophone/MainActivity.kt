package org.tom.monadophone

import android.Manifest
import android.content.pm.ActivityInfo
import android.content.pm.PackageManager
import android.content.res.Configuration
import android.opengl.GLSurfaceView
import android.os.Bundle
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.LocalActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Checkbox
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TextField
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.keepScreenOn
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.viewinterop.AndroidView
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import com.google.ar.core.ArCoreApk
import org.tom.monadophone.ui.theme.MonadoPhoneTheme

class MainActivity : ComponentActivity() {
    private var driver: MonadoDriver? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()

        // Set screen orientation to landscape only on first launch
        if (savedInstanceState == null) {
            requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE
        }

        // Set window options
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        val layoutParams = window.attributes
        layoutParams.screenBrightness = 1.0f
        window.attributes = layoutParams

        driver = MonadoDriver(this)

        Settings.init(this)

        setContent {
            MonadoPhoneTheme {
                App(driver = driver!!)
            }
        }
    }

    override fun onResume() {
        super.onResume()

        // No system bars
        @Suppress("DEPRECATION")
        window.decorView.systemUiVisibility =
            (View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY or View.SYSTEM_UI_FLAG_FULLSCREEN or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION)

        if (!hasCameraPermission()) {
            ActivityCompat.requestPermissions(
                this, arrayOf(Manifest.permission.CAMERA), 0
            )
            return
        }

        startArCore()
    }

    override fun onPause() {
        driver?.pause()
        super.onPause()
    }

    override fun onDestroy() {
        driver?.destroy()
        super.onDestroy()
    }

    override fun onRequestPermissionsResult(
        requestCode: Int, permissions: Array<String>, grantResults: IntArray
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == 0 && grantResults.isNotEmpty() && grantResults[0] == PackageManager.PERMISSION_GRANTED) {
            startArCore()
        }
    }

    private fun hasCameraPermission(): Boolean = ContextCompat.checkSelfPermission(
        this, Manifest.permission.CAMERA
    ) == PackageManager.PERMISSION_GRANTED

    private fun startArCore() {
        val installState = ArCoreApk.getInstance().requestInstall(this, true)
        if (installState == ArCoreApk.InstallStatus.INSTALLED) {
            driver?.resume()
        }
    }
}

@Composable
fun App(driver: MonadoDriver, modifier: Modifier = Modifier) {
    var showSettings by rememberSaveable { mutableStateOf(false) }
    val context = LocalContext.current
    val activity = LocalActivity.current as MainActivity

    if (ArCoreApk.getInstance().checkAvailability(context).isUnsupported()) {
        Text("ARCore is not supported on this device.")
        return
    }

    // Create Surface to render the stream.
    Box(
        modifier = modifier
            .fillMaxSize()
            .keepScreenOn()
            .clickable {
                driver.recenter()
            }
    ) {
        // GL surface driving the ARCore session, sits behind the video surface.
        AndroidView(
            factory = { ctx ->
                GLSurfaceView(ctx).apply {
                    driver.setGlSurfaceView(this)
                }
            }, modifier = Modifier.fillMaxSize()
        )
        AndroidView(
            factory = { ctx ->
                SurfaceView(ctx).apply {
                    holder.addCallback(object : SurfaceHolder.Callback {
                        override fun surfaceCreated(holder: SurfaceHolder) {
                            if (resources.configuration.orientation == Configuration.ORIENTATION_LANDSCAPE) {
                                driver.start(holder.surface)
                            }
                        }

                        override fun surfaceDestroyed(holder: SurfaceHolder) {
                            driver.destroy()
                        }

                        override fun surfaceChanged(
                            holder: SurfaceHolder, format: Int, width: Int, height: Int
                        ) {
                        }
                    })
                }
            }, modifier = Modifier.fillMaxSize()
        )
        IconButton(
            onClick = { driver.restart() }, modifier = Modifier.align(Alignment.TopStart)
        ) {
            Icon(
                painter = painterResource(R.drawable.reload),
                contentDescription = "reload",
                tint = Color.Unspecified
            )
        }
        IconButton(
            onClick = {
                showSettings = true
                activity.requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_PORTRAIT
            }, modifier = Modifier.align(Alignment.TopEnd)
        ) {
            Icon(
                painter = painterResource(R.drawable.settings),
                contentDescription = "settings",
                tint = Color.Unspecified
            )
        }
    }
    if (showSettings) {
        AlertDialog(
            onDismissRequest = {
                showSettings = false
                activity.requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE
            },
            title = { Text(text = "Settings") },
            text = {
                Column(
                    modifier = Modifier.verticalScroll(rememberScrollState())
                ) {
                    Text(
                        "Features",
                        fontSize = MaterialTheme.typography.titleMedium.fontSize
                    )
                    Row(
                        modifier = Modifier
                            .fillMaxWidth()
                            .clickable { Settings.enableHandTracking = !Settings.enableHandTracking },
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        Checkbox(
                            checked = Settings.enableHandTracking,
                            onCheckedChange = { Settings.enableHandTracking = it }
                        )
                        Text("Enable hand tracking")
                    }
                    Row(
                        modifier = Modifier
                            .fillMaxWidth()
                            .clickable { Settings.enable6DOFTracking = !Settings.enable6DOFTracking },
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        Checkbox(
                            checked = Settings.enable6DOFTracking,
                            onCheckedChange = { Settings.enable6DOFTracking = it }
                        )
                        Text("Enable 6DOF tracking")
                    }
                    Text(
                        "Network configuration",
                        fontSize = MaterialTheme.typography.titleMedium.fontSize
                    )
                    TextField(
                        value = Settings.multicastAddr,
                        onValueChange = { Settings.multicastAddr = it },
                        label = { Text("Multicast address") }
                    )
                    TextField(
                        value = Settings.port.toString(),
                        onValueChange = { Settings.port = it.toInt() },
                        label = { Text("Discovery port") }
                    )
                    TextField(
                        value = Settings.configPort.toString(),
                        onValueChange = { Settings.configPort = it.toInt() },
                        label = { Text("Config port") }
                    )
                    TextField(
                        value = Settings.streamPort.toString(),
                        onValueChange = { Settings.streamPort = it.toInt() },
                        label = { Text("Stream port") }
                    )
                    TextField(
                        value = Settings.posePort.toString(),
                        onValueChange = { Settings.posePort = it.toInt() },
                        label = { Text("Pose port") }
                    )
                    TextField(
                        value = Settings.handsPort.toString(),
                        onValueChange = { Settings.handsPort = it.toInt() },
                        label = { Text("Hands port") }
                    )
                }
            },
            confirmButton = {
                TextButton(onClick = {
                    showSettings = false
                    activity.requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE
                }) {
                    Text("Close")
                }
            }
        )
    }
}

@Preview(showBackground = true, showSystemUi = true)
@Composable
fun AppPreview() {
    MonadoPhoneTheme {
        App(driver = MonadoDriver(LocalContext.current))
    }
}
