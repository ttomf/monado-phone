package org.tom.monadophone

import android.content.Context
import android.util.Log
import com.google.mediapipe.framework.image.ByteBufferImageBuilder
import com.google.mediapipe.framework.image.MPImage
import com.google.mediapipe.tasks.core.BaseOptions
import com.google.mediapipe.tasks.core.Delegate
import com.google.mediapipe.tasks.vision.core.RunningMode
import com.google.mediapipe.tasks.vision.handlandmarker.HandLandmarker
import com.google.mediapipe.tasks.vision.handlandmarker.HandLandmarkerResult
import java.nio.ByteBuffer
import java.nio.ByteOrder

private const val TAG = "HandTracker"

const val HAND_WIDTH = 854
const val HAND_HEIGHT = 480

data class HandLandmarks(
    var timestampNs: Long,
    var leftHand: FloatArray? = null,
    var rightHand: FloatArray? = null
)

/**
 * Wire format of the hand packets sent to the PC
 *
 *  - int64  timestamp_ns
 *  - int8  flags (bit 0 = left hand, bit 1 = right hand)
 *  - float[63]?  left_hand (3*21)
 *  - float[63]?  right_hand (3*21)
 */
object HandsPacket {
    fun encode(landmarks: HandLandmarks): ByteArray {
        return ByteBuffer.allocate(
            9 + (if (landmarks.leftHand != null) 252 else 0) + (if (landmarks.rightHand != null) 252 else 0)
        ).order(ByteOrder.LITTLE_ENDIAN).apply {
            putLong(landmarks.timestampNs)
            var flags = 0
            if (landmarks.leftHand != null) flags = flags or 1
            if (landmarks.rightHand != null) flags = flags or 2
            put(flags.toByte())
            landmarks.leftHand?.forEach { putFloat(it) }
            landmarks.rightHand?.forEach { putFloat(it) }
        }.array()
    }
}

class HandTracker(
    val context: Context,
    private val processResult: (result: HandLandmarkerResult) -> Unit
) {
    private var handLandmarker: HandLandmarker? = null

    fun setup() {
        // Guard against a concurrent processImage() on the GL thread (called
        // during a restart) closing the landmarker under it -> use-after-free
        // in MediaPipe (SIGSEGV in nativeCreateCpuImage). The close is done
        // under the same lock as processImage() so they can never overlap.
        synchronized(this) {
            handLandmarker?.close()
            handLandmarker = null
        }

        val baseOptions = BaseOptions.builder()
            .setDelegate(Delegate.GPU)
            .setModelAssetPath("hand_landmarker.task")
            .build()
        val options = HandLandmarker.HandLandmarkerOptions.builder()
            .setBaseOptions(baseOptions)
            .setNumHands(2)
            .setMinHandDetectionConfidence(0.5f)
            .setMinTrackingConfidence(0.3f)
            .setRunningMode(RunningMode.LIVE_STREAM)
            .setResultListener { result, _ ->
                processResult(result)
            }.setErrorListener { error ->
                error.printStackTrace()
            }.build()
        Log.d(TAG, "Initializing Hand Landmarker...")
        val landmarker = HandLandmarker.createFromOptions(context, options)
        synchronized(this) {
            handLandmarker = landmarker
        }
    }

    fun processImage(rgba: ByteBuffer, width: Int, height: Int, timestampNs: Long) {
        synchronized(this) {
            val copy = ByteBuffer.allocateDirect(rgba.capacity()).order(ByteOrder.nativeOrder())
            rgba.position(0)
            copy.put(rgba)
            copy.position(0)
            val mpImage = ByteBufferImageBuilder(copy, width, height, MPImage.IMAGE_FORMAT_RGBA).build()
            handLandmarker?.detectAsync(mpImage, timestampNs / 1_000_000)
        }
    }

    fun close() {
        synchronized(this) {
            handLandmarker?.close()
            handLandmarker = null
        }
    }
}
