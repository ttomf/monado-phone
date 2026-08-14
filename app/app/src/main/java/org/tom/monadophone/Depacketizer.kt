package org.tom.monadophone

import java.io.ByteArrayOutputStream

/**
 * Depacketizes HEVC RTP payloads (RFC 7798) into complete NAL units with
 * 4-byte start codes, ready to be fed to a decoder.
 *
 * Handles single NAL unit packets (types 1-47), aggregation packets (type 48)
 * and fragmentation units (type 49).
 *
 * Not thread-safe; use from a single thread.
 */
class Depacketizer {
    private val fragment = ByteArrayOutputStream()

    /**
     * Processes one RTP payload (the bytes after the RTP header).
     *
     * @param payload RTP payload, the 2-byte HEVC NAL header included.
     * @return complete NAL units with a 4-byte start code prefix; empty when
     *         the payload carried no complete NAL unit (a fragment that is
     *         not the last one, an unknown packet type, or garbage).
     */
    fun depacketize(payload: ByteArray): List<ByteArray> {
        if (payload.size < 2) {
            return emptyList()
        }

        // HEVC 2-byte NAL header, the type is in the first byte.
        val nalType = (payload[0].toInt() shr 1) and 0x3F

        return when {
            // Single NAL unit packet.
            nalType <= 47 -> listOf(withStartCode(payload))

            // Aggregation packet.
            nalType == 48 -> depacketizeAggregation(payload)

            // Fragmentation unit.
            nalType == 49 -> depacketizeFragment(payload)

            else -> emptyList()
        }
    }

    private fun depacketizeAggregation(payload: ByteArray): List<ByteArray> {
        val nals = mutableListOf<ByteArray>()
        var offset = 2
        while (offset + 2 < payload.size) {
            // Each NAL is prefixed with a 2-byte big-endian size.
            val size =
                ((payload[offset].toInt() and 0xFF) shl 8) or (payload[offset + 1].toInt() and 0xFF)
            if (size < 2 || offset + 2 + size > payload.size) {
                break
            }
            nals.add(withStartCode(payload, offset + 2, size))
            offset += 2 + size
        }
        return nals
    }

    private fun depacketizeFragment(payload: ByteArray): List<ByteArray> {
        if (payload.size < 3) {
            return emptyList()
        }
        val fuHeader = payload[2].toInt()
        val start = (fuHeader and 0x80) != 0
        val end = (fuHeader and 0x40) != 0
        val originalType = fuHeader and 0x3F

        if (start) {
            // Rebuild the original 2-byte NAL header: the payload header byte
            // with the type replaced, and the unchanged second byte.
            fragment.reset()
            fragment.write(START_CODE)
            fragment.write((payload[0].toInt() and 0x81) or (originalType shl 1))
            fragment.write(payload[1].toInt())
        }
        if (fragment.size() > 0) {
            fragment.write(payload, 3, payload.size - 3)
        }

        if (!end || fragment.size() <= 0) {
            return emptyList()
        }

        val nal = fragment.toByteArray()
        fragment.reset()
        return listOf(nal)
    }

    private fun withStartCode(nal: ByteArray, offset: Int = 0, length: Int = nal.size): ByteArray {
        val result = ByteArray(4 + length)
        START_CODE.copyInto(result)
        nal.copyInto(result, 4, offset, offset + length)
        return result
    }

    private companion object {
        /** 4-byte HEVC NAL start code. */
        val START_CODE = byteArrayOf(0, 0, 0, 1)
    }
}
