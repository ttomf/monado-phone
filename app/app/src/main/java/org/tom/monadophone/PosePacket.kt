package org.tom.monadophone

import java.nio.ByteBuffer
import java.nio.ByteOrder

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
    const val SIZE = 40

    /** Encodes an [ArPose] into a new [SIZE]-byte packet. */
    fun encode(pose: ArPose): ByteArray {
        return ByteBuffer.allocate(SIZE).order(ByteOrder.LITTLE_ENDIAN).apply {
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
