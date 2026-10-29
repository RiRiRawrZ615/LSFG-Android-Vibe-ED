package com.lsfg.fork.capture

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Canvas
import android.graphics.ColorSpace
import android.graphics.PixelFormat
import android.hardware.HardwareBuffer
import com.lsfg.fork.wsi.WsiBridge
import com.topjohnwu.superuser.Shell
import com.topjohnwu.superuser.ShellUtils
import kotlin.concurrent.thread

/**
 * Root-first screen capture engine.
 *
 * v1 approach: run `screencap <file>` (default = raw RGBA8888 framebuffer)
 * through the libsu root shell on a background thread, wrap the raw bytes
 * into a Bitmap -> RGBA8888 [HardwareBuffer], and push the native AHB pointer
 * to the WSI presenter.
 *
 * Capture rate is capped by [minIntervalMs] (screencap over root takes
 * ~15-30ms). This is the "ship first, optimize later" path; a zero-copy
 * ScreenCapture/ADB binder path is a TODO for the next session.
 */
class CaptureEngine(
    private val width: Int,
    private val height: Int,
    private val minIntervalMs: Long = 24,
    private val onStats: (framesPerSec: Float, avgLatencyMs: Float) -> Unit = { _, _ -> }
) {

    @Volatile
    var running: Boolean = false
        private set

    private var worker: Thread? = null
    private val lock = Any()
    private val pngPath = "/data/local/tmp/lsfg_cap.png"

    /**
     * Optional frame-generation sink. When set, each captured source frame is
     * handed to this callback (with its native AHB pointer) to produce the
     * frame(s) to present. Return the number of frames pushed to the presenter
     * (0 means "produce nothing / let the caller fall back to passthrough").
     * When null, the captured frame is pushed directly (passthrough).
     */
    @Volatile
    var frameSink: ((srcAhb: HardwareBuffer, srcAhbPtr: Long) -> Int)? = null

    @Volatile private var frames = 0
    @Volatile private var latencySum = 0L
    @Volatile private var lastStatsTime = 0L
    @Volatile private var lastError: String? = null

    fun start() {
        synchronized(lock) {
            if (running) return
            running = true
            frames = 0
            latencySum = 0
            lastError = null
            lastStatsTime = System.currentTimeMillis()

            worker = thread(name = "lsfg-capture") {
                try { ShellUtils.fastCmd("rm -f $pngPath") } catch (_: Throwable) {}
                while (running) {
                    val t0 = System.currentTimeMillis()
                    try {
                        val ahb = captureOnce()
                        if (ahb != null) {
                            val ahbPtr = WsiBridge.nativeAhbPtr(ahb)
                            val sink = frameSink
                            if (sink != null) {
                                // Drive the frame-generation pipeline; it pushes the
                                // generated frame(s) to the presenter and reports how many.
                                val produced = try {
                                    sink(ahb, ahbPtr)
                                } catch (t: Throwable) {
                                    lastError = t.message ?: "framegen failed"
                                    0
                                }
                                if (produced > 0) {
                                    frames++
                                    latencySum += System.currentTimeMillis() - t0
                                } else if (ahbPtr != 0L) {
                                    // Framegen unavailable: fall back to passthrough.
                                    WsiBridge.nativePushFrame(ahbPtr)
                                    frames++
                                    latencySum += System.currentTimeMillis() - t0
                                }
                            } else if (ahbPtr != 0L) {
                                WsiBridge.nativePushFrame(ahbPtr)
                                frames++
                                latencySum += System.currentTimeMillis() - t0
                            }
                            ahb.close()
                        }
                    } catch (t: Throwable) {
                        lastError = t.message ?: "capture failed"
                    }
                    // Pacing.
                    val elapsed = System.currentTimeMillis() - t0
                    val sleepMs = minIntervalMs - elapsed
                    if (sleepMs > 0) {
                        try { Thread.sleep(sleepMs) } catch (_: InterruptedException) { break }
                    }
                    // Stats every second.
                    val now = System.currentTimeMillis()
                    if (now - lastStatsTime >= 1000L) {
                        val dt = (now - lastStatsTime) / 1000f
                        onStats(frames / dt, if (frames > 0) latencySum / frames.toFloat() else 0f)
                        frames = 0
                        latencySum = 0
                        lastStatsTime = now
                    }
                }
            }
        }
    }

    fun stop() {
        synchronized(lock) {
            if (!running) return
            running = false
        }
        try { worker?.interrupt() } catch (_: Throwable) {}
        try { worker?.join(600) } catch (_: Throwable) {}
        worker = null
        try { ShellUtils.fastCmd("rm -f $pngPath") } catch (_: Throwable) {}
    }

    fun lastError(): String? = lastError

    /** One capture: PNG screencap -> decode -> scale -> HardwareBuffer. */
    private fun captureOnce(): HardwareBuffer? {
        val result = Shell.cmd("screencap -p $pngPath").exec()
        if (!result.isSuccess) return null

        val png = readFileB64() ?: return null
        val src = BitmapFactory.decodeByteArray(png, 0, png.size) ?: return null

        // Scale/crop to the target overlay size.
        val fitted: Bitmap = if (src.width != width || src.height != height)
            Bitmap.createScaledBitmap(src, width, height, true) ?: src
        else src
        if (fitted !== src) src.recycle()

        val ahb = HardwareBuffer.create(
            width, height,
            PixelFormat.RGBA_8888,
            1,
            HardwareBuffer.USAGE_GPU_SAMPLED_IMAGE or HardwareBuffer.USAGE_CPU_READ_RARELY
        )
        // Write pixels into the AHB-backed hardware bitmap via Canvas.
        val hw = Bitmap.wrapHardwareBuffer(ahb, ColorSpace.get(ColorSpace.Named.SRGB)) ?: return null
        Canvas(hw).drawBitmap(fitted, 0f, 0f, null)
        hw.recycle()
        fitted.recycle()
        return ahb
    }

    private fun readFileB64(): ByteArray? {
        // Root transport: base64 over the shell keeps binary integrity.
        val out = Shell.cmd("base64 -w 0 $pngPath").exec()
        if (!out.isSuccess) return null
        val b64 = out.getOut().joinToString("")
        if (b64.isEmpty()) return null
        return try {
            android.util.Base64.decode(b64, android.util.Base64.DEFAULT)
        } catch (_: Throwable) {
            null
        }
    }
}
