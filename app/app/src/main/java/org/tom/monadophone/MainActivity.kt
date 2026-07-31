package org.tom.monadophone

import android.app.Activity
import android.content.pm.ActivityInfo
import android.content.res.Configuration
import android.os.Bundle
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.keepScreenOn
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.viewinterop.AndroidView
import org.tom.monadophone.ui.theme.MonadoPhoneTheme

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent {
            MonadoPhoneTheme {
                App()
            }
        }
    }
}

@Composable
@Preview(showBackground = true, showSystemUi = true)
fun App(modifier: Modifier = Modifier) {
    // Initialize variables
    val driver = remember { MonadoDriver() }
    val view = LocalView.current
    val context = LocalContext.current
    val window = (context as Activity).window

    // Dispose effects fullscreen, landscape, keep screen on and max screen brightness
    @Suppress("DEPRECATION") DisposableEffect(Unit) {
        view.systemUiVisibility =
            (View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY or View.SYSTEM_UI_FLAG_FULLSCREEN or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION)
        onDispose {
            view.systemUiVisibility = View.SYSTEM_UI_FLAG_VISIBLE
        }
    }
    DisposableEffect(Unit) {
        val activity = context as ComponentActivity
        activity.requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE
        onDispose {
            activity.requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED
        }
    }
    window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
    val layoutParams = window.attributes
    layoutParams.screenBrightness = 1.0f
    window.attributes = layoutParams

    // Create Surface to render the stream.
    Box(
        Modifier
            .fillMaxSize()
            .keepScreenOn()
    ) {
        AndroidView(
            factory = { context ->
                SurfaceView(context).apply {
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
    }
}
