package org.tom.monadophone

import android.content.Context
import android.opengl.GLES11Ext
import android.opengl.GLES20
import android.opengl.GLSurfaceView
import android.util.Log
import com.google.ar.core.Config
import com.google.ar.core.Frame
import com.google.ar.core.Session
import com.google.ar.core.exceptions.NotYetAvailableException
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.FloatBuffer
import java.util.concurrent.atomic.AtomicReference
import javax.microedition.khronos.egl.EGLConfig
import javax.microedition.khronos.opengles.GL10

/**
 * A single ARCore pose sample in display-oriented coordinates.
 */
data class ArPose(
    val timestampNs: Long,
    val trackingState: Int,
    val qx: Float,
    val qy: Float,
    val qz: Float,
    val qw: Float,
    val tx: Float,
    val ty: Float,
    val tz: Float,
)

private const val TAG = "ArCorePose"

/**
 * ARCore session with a GL surface showing the camera feed.
 */
class ArCorePose(
    private val context: Context,
    private val surfaceView: GLSurfaceView,
) {
    private val latest = AtomicReference<ArPose?>(null)

    @Volatile
    private var session: Session? = null

    private var resumed = false

    private var textureId = -1
    private var textureSet = false

    private var surfaceW = 0
    private var surfaceH = 0
    private var surfaceRotation = 0

    // Camera preview renderer state.
    private var program = 0
    private var positionAttrib = -1
    private var texCoordAttrib = -1
    private var textureUniform = -1
    private var positionBuffer: FloatBuffer? = null
    private var baseTexCoords: FloatBuffer? = null
    private var displayTexCoords: FloatBuffer? = null

    init {
        surfaceView.setEGLContextClientVersion(2)
        surfaceView.setRenderer(object : GLSurfaceView.Renderer {
            override fun onSurfaceCreated(gl: GL10?, config: EGLConfig?) {
                val textures = IntArray(1)
                GLES20.glGenTextures(1, textures, 0)
                textureId = textures[0]
                textureSet = false
                setupCameraPreview()
            }

            override fun onSurfaceChanged(gl: GL10?, width: Int, height: Int) {
                GLES20.glViewport(0, 0, width, height)
                surfaceW = width
                surfaceH = height
                surfaceRotation = surfaceView.display.rotation
                session?.setDisplayGeometry(surfaceRotation, width, height)
            }

            override fun onDrawFrame(gl: GL10?) {
                val sess = session
                if (sess == null) {
                    clear()
                    return
                }
                if (textureId != -1 && !textureSet) {
                    try {
                        sess.setCameraTextureName(textureId)
                        textureSet = true
                    } catch (e: Exception) {
                        Log.e(TAG, "setCameraTextureName failed", e)
                    }
                }
                try {
                    val frame: Frame = sess.update()
                    val camera = frame.camera
                    val pose = camera.displayOrientedPose
                    latest.set(
                        ArPose(
                            timestampNs = frame.timestamp,
                            trackingState = camera.trackingState.ordinal,
                            qx = pose.qx(),
                            qy = pose.qy(),
                            qz = pose.qz(),
                            qw = pose.qw(),
                            tx = pose.tx(),
                            ty = pose.ty(),
                            tz = pose.tz(),
                        )
                    )
                    drawCameraBackground(frame)
                } catch (e: Exception) {
                    // The first frames are not yet available, this is normal.
                    clear()
                    if (e !is NotYetAvailableException) {
                        Log.e(TAG, "session.update failed", e)
                    }
                }
            }
        })
        surfaceView.renderMode = GLSurfaceView.RENDERMODE_CONTINUOUSLY
        // Start the GL thread immediately so that a surface created during an
        // activity relaunch can never race a later onResume() call.
        // GLSurfaceView crashes with a null GL thread in that case.
        surfaceView.onResume()
    }

    fun resume() {
        // No-op when the GL thread is already running.
        surfaceView.onResume()
        if (session == null) {
            try {
                session = Session(context).also {
                    it.configure(Config(it))
                    if (surfaceW > 0) {
                        it.setDisplayGeometry(surfaceRotation, surfaceW, surfaceH)
                    }
                }
            } catch (e: Exception) {
                Log.e(TAG, "session create failed", e)
            }
        }
        if (!resumed) {
            try {
                session?.resume()
                resumed = true
            } catch (e: Exception) {
                Log.e(TAG, "session resume failed", e)
            }
        }
    }

    fun pause() {
        if (resumed) {
            try {
                session?.pause()
            } catch (e: Exception) {
                // SessionPausedException when already paused, ignore.
            }
            resumed = false
        }
    }

    fun close() {
        pause()
        try {
            session?.close()
        } catch (e: Exception) {
            // Ignore, the session is being closed.
        }
        session = null
        resumed = false
        latest.set(null)
        surfaceView.onPause()
    }

    fun latestPose(): ArPose? = latest.get()

    private fun clear() {
        GLES20.glClearColor(0f, 0f, 0f, 1f)
        GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT)
    }

    private fun drawCameraBackground(frame: Frame) {
        if (!textureSet || textureId == -1 || program == 0) {
            clear()
            return
        }
        val posBuf = positionBuffer ?: return
        val base = baseTexCoords ?: return
        val display = displayTexCoords ?: return

        // Correctly orient the camera image for the current display rotation.
        base.position(0)
        display.position(0)
        frame.transformDisplayUvCoords(base, display)
        display.position(0)

        GLES20.glUseProgram(program)
        GLES20.glActiveTexture(GLES20.GL_TEXTURE0)
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, textureId)
        GLES20.glUniform1i(textureUniform, 0)

        posBuf.position(0)
        GLES20.glEnableVertexAttribArray(positionAttrib)
        GLES20.glVertexAttribPointer(positionAttrib, 3, GLES20.GL_FLOAT, false, 12, posBuf)

        GLES20.glEnableVertexAttribArray(texCoordAttrib)
        GLES20.glVertexAttribPointer(texCoordAttrib, 2, GLES20.GL_FLOAT, false, 8, display)

        GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4)

        GLES20.glDisableVertexAttribArray(positionAttrib)
        GLES20.glDisableVertexAttribArray(texCoordAttrib)
    }

    private fun setupCameraPreview() {
        val vertexSrc = """
            attribute vec4 a_Position;
            attribute vec2 a_TexCoord;
            varying vec2 v_TexCoord;
            void main() {
                gl_Position = a_Position;
                v_TexCoord = a_TexCoord;
            }
        """.trimIndent()
        val fragmentSrc = """
            #extension GL_OES_EGL_image_external : require
            precision mediump float;
            uniform samplerExternalOES u_Texture;
            varying vec2 v_TexCoord;
            void main() {
                gl_FragColor = texture2D(u_Texture, v_TexCoord);
            }
        """.trimIndent()

        val vertex = compileShader(GLES20.GL_VERTEX_SHADER, vertexSrc)
        val fragment = compileShader(GLES20.GL_FRAGMENT_SHADER, fragmentSrc)
        if (vertex == 0 || fragment == 0) {
            program = 0
            return
        }

        program = GLES20.glCreateProgram()
        GLES20.glAttachShader(program, vertex)
        GLES20.glAttachShader(program, fragment)
        GLES20.glLinkProgram(program)
        GLES20.glDeleteShader(vertex)
        GLES20.glDeleteShader(fragment)

        val linkStatus = IntArray(1)
        GLES20.glGetProgramiv(program, GLES20.GL_LINK_STATUS, linkStatus, 0)
        if (linkStatus[0] == 0) {
            Log.e(TAG, "background program link failed: ${GLES20.glGetProgramInfoLog(program)}")
            program = 0
            return
        }

        positionAttrib = GLES20.glGetAttribLocation(program, "a_Position")
        texCoordAttrib = GLES20.glGetAttribLocation(program, "a_TexCoord")
        textureUniform = GLES20.glGetUniformLocation(program, "u_Texture")

        // Full-screen quad, drawn as a triangle strip.
        positionBuffer = directFloatBuffer(
            floatArrayOf(
                -1f, -1f, 0f,
                -1f, 1f, 0f,
                1f, -1f, 0f,
                1f, 1f, 0f,
            )
        )

        // Base texture coordinates, transformed per frame to match display.
        baseTexCoords = directFloatBuffer(floatArrayOf(0f, 1f, 0f, 0f, 1f, 1f, 1f, 0f))
        displayTexCoords = directFloatBuffer(FloatArray(8))
    }

    private fun compileShader(type: Int, source: String): Int {
        val shader = GLES20.glCreateShader(type)
        GLES20.glShaderSource(shader, source)
        GLES20.glCompileShader(shader)
        val ok = IntArray(1)
        GLES20.glGetShaderiv(shader, GLES20.GL_COMPILE_STATUS, ok, 0)
        if (ok[0] == 0) {
            Log.e(TAG, "shader compile failed: ${GLES20.glGetShaderInfoLog(shader)}")
            GLES20.glDeleteShader(shader)
            return 0
        }
        return shader
    }

    private fun directFloatBuffer(data: FloatArray): FloatBuffer {
        val buffer = ByteBuffer.allocateDirect(data.size * 4).order(ByteOrder.nativeOrder()).asFloatBuffer()
        buffer.put(data)
        buffer.position(0)
        return buffer
    }
}
