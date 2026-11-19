package com.danmilk.ipodify

import android.graphics.Bitmap
import android.util.Base64
import kotlin.math.roundToInt

object AlbumArtProcessor {

    /**
     * Converts a source Bitmap into a 48x48 1-bit monochrome XBM-compatible byte array (288 bytes)
     * using Floyd-Steinberg error diffusion dithering.
     */
    fun processTo1BitXbm(source: Bitmap, targetW: Int = 48, targetH: Int = 48): ByteArray {
        val scaled = Bitmap.createScaledBitmap(source, targetW, targetH, true)
        val gray = Array(targetH) { FloatArray(targetW) }

        for (y in 0 until targetH) {
            for (x in 0 until targetW) {
                val pixel = scaled.getPixel(x, y)
                val r = (pixel shr 16) and 0xFF
                val g = (pixel shr 8) and 0xFF
                val b = pixel and 0xFF
                gray[y][x] = (0.299f * r + 0.587f * g + 0.114f * b)
            }
        }

        val bytesPerRow = (targetW + 7) / 8 // 6 bytes per row for 48px
        val result = ByteArray(bytesPerRow * targetH) // 288 bytes

        for (y in 0 until targetH) {
            for (x in 0 until targetW) {
                val oldVal = gray[y][x].coerceIn(0f, 255f)
                val newVal = if (oldVal > 127f) 255f else 0f
                val err = oldVal - newVal

                // In U8g2 XBM format, LSB is the leftmost pixel of each byte
                if (newVal > 0f) {
                    val byteIdx = y * bytesPerRow + (x / 8)
                    val bitIdx = x % 8
                    result[byteIdx] = (result[byteIdx].toInt() or (1 shl bitIdx)).toByte()
                }

                // Floyd-Steinberg error diffusion
                if (x + 1 < targetW) gray[y][x + 1] += err * (7f / 16f)
                if (y + 1 < targetH) {
                    if (x - 1 >= 0) gray[y + 1][x - 1] += err * (3f / 16f)
                    gray[y + 1][x] += err * (5f / 16f)
                    if (x + 1 < targetW) gray[y + 1][x + 1] += err * (1f / 16f)
                }
            }
        }

        return result
    }

    fun toBase64(bytes: ByteArray): String {
        return Base64.encodeToString(bytes, Base64.NO_WRAP)
    }
}
