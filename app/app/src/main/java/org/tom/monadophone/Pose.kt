package org.tom.monadophone

import android.content.Context
import android.opengl.GLES11Ext
import android.opengl.GLES20
import android.opengl.GLSurfaceView
import android.util.Log
import com.google.ar.core.Config
import com.google.ar.core.Frame
import com.google.ar.core.Pose
import com.google.ar.core.Session
import com.google.ar.core.exceptions.NotYetAvailableException
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.FloatBuffer
import java.util.concurrent.atomic.AtomicReference
import javax.microedition.khronos.egl.EGLConfig
import javax.microedition.khronos.opengles.GL10
import kotlin.math.atan2
import kotlin.math.cos
import kotlin.math.sin

/**
 * A single ARCore pose sample in display-oriented coordinates.
 */
data class ArPose(
    val timestampNs: Long,
    val trackingState: Int,
    var qx: Float,
    var qy: Float,
    var qz: Float,
    var qw: Float,
    var tx: Float,
    var ty: Float,
    var tz: Float,
) {
    operator fun minus(other: ArPose): ArPose {
        val yawOther = atan2(
            2f * (other.qw * other.qy + other.qx * other.qz),
            1f - 2f * (other.qy * other.qy + other.qz * other.qz)
        )
        val halfAngle = yawOther * 0.5f
        val yawRotation =
            Pose.makeRotation(0f, sin(halfAngle), 0f, cos(halfAngle))
        val translation = Pose.makeTranslation(other.tx, other.ty, other.tz)
        val originPose = translation.compose(yawRotation)

        val thisPose = Pose(
            floatArrayOf(tx, ty, tz),
            floatArrayOf(qx, qy, qz, qw)
        )

        val relativePose = originPose.inverse().compose(thisPose)
        val t = relativePose.translation
        val q = relativePose.rotationQuaternion
        return ArPose(
            timestampNs = this.timestampNs,
            trackingState = this.trackingState,
            qx = q[0], qy = q[1], qz = q[2], qw = q[3],
            tx = t[0], ty = t[1], tz = t[2]
        )
    }
}


/**
 * Wire format of the pose packets sent to the PC, 40 bytes little-endian:
 *
 *  - int64  timestamp_ns    (phone clock, unused by the PC for now)
 *  - int32  tracking_state  (0 = tracking, 1 = paused, 2 = stopped)
 *  - float  qx, qy, qz, qw
 *  - float  tx, ty, tz
 *
 * Mirrors phone_packet.c in the Monado phone driver, keep both in sync.
 */
object PosePacket {
    fun encode(pose: ArPose): ByteArray {
        return ByteBuffer.allocate(40).order(ByteOrder.LITTLE_ENDIAN).apply {
            putLong(pose.timestampNs)
            putInt(pose.trackingState)
            putFloat(pose.qx)
            putFloat(pose.qy)
            putFloat(pose.qz)
            putFloat(pose.qw)
            putFloat(pose.tx)
            putFloat(pose.ty)
            putFloat(pose.tz)
        }.array()
    }
}

private const val TAG = "ArCorePose"

/**
 * ARCore session with a GL surface showing the camera feed.
 */
class ArCorePose(
    private val context: Context,
    private val surfaceView: GLSurfaceView,
    private val handTracker: HandTracker
) {
    private val latest = AtomicReference<ArPose?>(null)

    // For recentering headset
    private var offset = AtomicReference(ArPose(0, 0, 0f, 0f, 0f, 1f, 0f, 0f, 0f))

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
    private var fboTexCoords: FloatBuffer? = null

    // Offscreen FBO for hand tracking (smaller than display to reduce glReadPixels cost).
    private var handFbo = -1
    private var handTex = -1
    private var handPixelBuffer: ByteBuffer? = null
    private var lastHandFrameNs = 0L
    private val handIntervalNs = 33_333_333L // ~30 Hz

    init {
        surfaceView.setEGLContextClientVersion(2)
        surfaceView.setRenderer(object : GLSurfaceView.Renderer {
            override fun onSurfaceCreated(gl: GL10?, config: EGLConfig?) {
                val textures = IntArray(1)
                GLES20.glGenTextures(1, textures, 0)
                textureId = textures[0]
                textureSet = false
                setupCameraPreview()
                setupHandFbo()
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
                    if (camera.trackingState == com.google.ar.core.TrackingState.TRACKING) {
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
                    } else {
                        latest.set(null)
                    }
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

    fun recenter() {
        val pose = latest.get() ?: return
        offset.set(pose)
    }

    fun resume() {
        // No-op when the GL thread is already running.
        surfaceView.onResume()
        if (session == null) {
            try {
                session = Session(context).also {
                    it.configure(Config(it).apply {
                        updateMode = Config.UpdateMode.LATEST_CAMERA_IMAGE
                    })
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
    fun offset(): ArPose = offset.get()

    private fun clear() {
        GLES20.glClearColor(0f, 0f, 0f, 1f)
        GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT)
    }

    private fun drawQuad(width: Int, height: Int, display: FloatBuffer) {
        val posBuf = positionBuffer ?: return

        GLES20.glViewport(0, 0, width, height)
        GLES20.glUseProgram(program)
        GLES20.glActiveTexture(GLES20.GL_TEXTURE0)
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, textureId)
        GLES20.glUniform1i(textureUniform, 0)

        posBuf.position(0)
        GLES20.glEnableVertexAttribArray(positionAttrib)
        GLES20.glVertexAttribPointer(positionAttrib, 3, GLES20.GL_FLOAT, false, 12, posBuf)

        display.position(0)
        GLES20.glEnableVertexAttribArray(texCoordAttrib)
        GLES20.glVertexAttribPointer(texCoordAttrib, 2, GLES20.GL_FLOAT, false, 8, display)

        GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4)

        GLES20.glDisableVertexAttribArray(positionAttrib)
        GLES20.glDisableVertexAttribArray(texCoordAttrib)
    }

    private fun drawCameraBackground(frame: Frame) {
        if (!textureSet || textureId == -1 || program == 0) {
            clear()
            return
        }
        val base = baseTexCoords ?: return
        val display = displayTexCoords ?: return

        base.position(0)
        display.position(0)
        frame.transformDisplayUvCoords(base, display)
        display.position(0)

        drawQuad(surfaceW, surfaceH, display)
        if (handFbo != -1 && frame.timestamp - lastHandFrameNs >= handIntervalNs) {
            lastHandFrameNs = frame.timestamp
            val buf = handPixelBuffer ?: return
            // Flip UVs for the FBO render so that the subsequent glReadPixels
            // (which reads rows bottom-up) yields a top-down image for
            // MediaPipe. GL does this flip, no per-frame CPU copy needed.
            val fboUv = fboTexCoords ?: return
            fboUv.position(0)
            display.position(0)
            for (i in 0 until 4) {
                fboUv.put(display.get())
                fboUv.put(1f - display.get())
            }
            fboUv.position(0)
            GLES20.glBindFramebuffer(GLES20.GL_FRAMEBUFFER, handFbo)
            drawQuad(HAND_WIDTH, HAND_HEIGHT, fboUv)
            buf.clear()
            GLES20.glReadPixels(
                0,
                0,
                HAND_WIDTH,
                HAND_HEIGHT,
                GLES20.GL_RGBA,
                GLES20.GL_UNSIGNED_BYTE,
                buf
            )
            GLES20.glBindFramebuffer(GLES20.GL_FRAMEBUFFER, 0)
            GLES20.glViewport(0, 0, surfaceW, surfaceH)
            buf.rewind()
            handTracker.processImage(buf, HAND_WIDTH, HAND_HEIGHT, frame.timestamp)
        }
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
        fboTexCoords = directFloatBuffer(FloatArray(8))
    }

    private fun setupHandFbo() {
        val fbo = IntArray(1)
        GLES20.glGenFramebuffers(1, fbo, 0)
        handFbo = fbo[0]

        val tex = IntArray(1)
        GLES20.glGenTextures(1, tex, 0)
        handTex = tex[0]
        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, handTex)
        GLES20.glTexImage2D(
            GLES20.GL_TEXTURE_2D, 0, GLES20.GL_RGBA, HAND_WIDTH, HAND_HEIGHT, 0,
            GLES20.GL_RGBA, GLES20.GL_UNSIGNED_BYTE, null
        )
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR)

        GLES20.glBindFramebuffer(GLES20.GL_FRAMEBUFFER, handFbo)
        GLES20.glFramebufferTexture2D(
            GLES20.GL_FRAMEBUFFER, GLES20.GL_COLOR_ATTACHMENT0, GLES20.GL_TEXTURE_2D, handTex, 0
        )
        val status = GLES20.glCheckFramebufferStatus(GLES20.GL_FRAMEBUFFER)
        if (status != GLES20.GL_FRAMEBUFFER_COMPLETE) {
            Log.e(TAG, "hand FBO incomplete: $status")
        }
        GLES20.glBindFramebuffer(GLES20.GL_FRAMEBUFFER, 0)
        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, 0)

        handPixelBuffer =
            ByteBuffer.allocateDirect(HAND_WIDTH * HAND_HEIGHT * 4).order(ByteOrder.nativeOrder())
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
        val buffer =
            ByteBuffer.allocateDirect(data.size * 4).order(ByteOrder.nativeOrder()).asFloatBuffer()
        buffer.put(data)
        buffer.position(0)
        return buffer
    }
}
