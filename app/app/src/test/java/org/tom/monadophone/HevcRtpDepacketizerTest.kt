package org.tom.monadophone

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class HevcRtpDepacketizerTest {
    private val depacketizer = HevcRtpDepacketizer()

    private val startCode = byteArrayOf(0, 0, 0, 1)

    /** Builds a 2-byte HEVC NAL header for the given type. */
    private fun nalHeader(type: Int): ByteArray = byteArrayOf((type shl 1).toByte(), 0)

    /** Builds a single NAL unit packet payload. */
    private fun single(type: Int, data: ByteArray = byteArrayOf()): ByteArray = nalHeader(type) + data

    /** Encodes a length as a 2-byte big-endian size. */
    private fun size2(length: Int): ByteArray = byteArrayOf((length shr 8).toByte(), length.toByte())

    @Test
    fun singleNalPacket_yieldsOneNalWithStartCode() {
        val payload = single(type = 19, data = byteArrayOf(0x01, 0x02, 0x03))

        val nals = depacketizer.depacketize(payload)

        assertEquals(1, nals.size)
        assertArrayEquals(startCode + payload, nals[0])
    }

    @Test
    fun aggregationPacket_yieldsAllNals() {
        val first = single(type = 19, data = byteArrayOf(1, 2, 3))
        val second = single(type = 20, data = byteArrayOf(4, 5))
        val payload = nalHeader(48) + size2(first.size) + first + size2(second.size) + second

        val nals = depacketizer.depacketize(payload)

        assertEquals(2, nals.size)
        assertArrayEquals(startCode + first, nals[0])
        assertArrayEquals(startCode + second, nals[1])
    }

    @Test
    fun truncatedAggregationPacket_yieldsTheCompleteNals() {
        val first = single(type = 19, data = byteArrayOf(1, 2, 3))
        val truncated = single(type = 20, data = byteArrayOf(4, 5))
        // Second NAL claims more data than is present.
        val payload = nalHeader(48) + size2(first.size) + first + size2(truncated.size + 100) + truncated

        val nals = depacketizer.depacketize(payload)

        assertEquals(1, nals.size)
        assertArrayEquals(startCode + first, nals[0])
    }

    @Test
    fun fragmentedNal_assemblesAfterTheLastFragment() {
        val data = byteArrayOf(1, 2, 3, 4, 5)
        // Fragmentation units carry the type 49 in their NAL header, the
        // original type lives in the FU header byte.
        val fuStart = byteArrayOf((0x80 or 21).toByte()) // start, type 21
        val fuMid = byteArrayOf(21)                      // no flags, type 21
        val fuEnd = byteArrayOf((0x40 or 21).toByte())   // end, type 21

        val first = depacketizer.depacketize(nalHeader(49) + fuStart + data.copyOfRange(0, 2))
        val middle = depacketizer.depacketize(nalHeader(49) + fuMid + data.copyOfRange(2, 4))
        val last = depacketizer.depacketize(nalHeader(49) + fuEnd + data.copyOfRange(4, 5))

        assertTrue(first.isEmpty())
        assertTrue(middle.isEmpty())
        assertEquals(1, last.size)
        assertArrayEquals(startCode + nalHeader(21) + data, last[0])
    }

    @Test
    fun fragmentWithoutStart_yieldsNothing() {
        val payload = nalHeader(49) + byteArrayOf(21) + byteArrayOf(1, 2)

        val nals = depacketizer.depacketize(payload)

        assertTrue(nals.isEmpty())
    }

    @Test
    fun shortPayload_yieldsNothing() {
        assertTrue(depacketizer.depacketize(byteArrayOf()).isEmpty())
        assertTrue(depacketizer.depacketize(byteArrayOf(0)).isEmpty())
    }

    @Test
    fun unknownPacketType_yieldsNothing() {
        val payload = nalHeader(50) + byteArrayOf(1, 2)

        val nals = depacketizer.depacketize(payload)

        assertTrue(nals.isEmpty())
    }
}
