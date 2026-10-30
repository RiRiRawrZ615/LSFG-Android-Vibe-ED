package com.lsfg.fork.capture

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Canvas
import android.graphics.ColorSpace
import android.graphics.PixelFormat
import android.hardware.display.VirtualDisplay
import android.hardware.HardwareBuffer
import android.media.ImageReader
import android.media.projection.MediaProjection
import com.lsfg.fork.wsi.WsiBridge
import com.topjohnwu.superuser.Shell
import com.topjohnwu.superuser.ShellUtils
import java.nio.ByteBuffer
import kotlin.concurrent.thread

/**
 * Screen capture engine with three modes:
 *  - "nonroot":  MediaProjection + VirtualDisplay + ImageReader (scrcpy-style).
 *  - "root":     root shell `screencap` -> decode -> HardwareBuffer (KernelSU).
 *  - "shizuku":  Shizuku shell `screencap` (privileged, no KernelSU).
 *
 * All modes write into a fixed ring of [HardwareBuffer] slots so the WSI
 * presenter only ever imports a small, stable set of Vulkan images (no per-frame
 * AHB import / GPU-memory growth).
 */
class CaptureEngine(
    private val appContext: Context,
    private val width: Int,
    private val height: Int,
    private val densityDpi: Int = 0,
    private val minIntervalMs: Long = 33,
    private val onStats: (framesPerSec: Float, avgLatencyMs: Float, err: String?) -> Unit = { _, _, _ -> },
    /** When set, each captured source frame (AHB + native ptr) drives framegen.
     *  Return the number of frames pushed (0 => passthrough fallback). */
    @Volatile var frameSink: ((srcAhb: HardwareBuffer, srcAhbPtr: Long) -> Int)? = null,
    /** MediaProjection for nonroot mode (null for root/shizuku). */
    private val projection: MediaProjection?
) {
    @Volatile var running: Boolean = false
        private set

    @Volatile var mode: String = "root"
    @Volatile var lastError: String? = null
    @Volatile var lastFrameSource: String = ""

    private var worker: Thread? = null
    private val lock = Any()
    private val pngPath = "/data/local/tmp/lsfg_cap.png"

    // ---- fixed capture ring (shared by all modes) ----
    private val ringSize = 6
    private val ring: Array<HardwareBuffer>
    private var ringIdx = 0
    init {
        ring = Array(ringSize) {
            HardwareBuffer.create(
                width, height, PixelFormat.RGBA_8888, 1,
                HardwareBuffer.USAGE_GPU_SAMPLED_IMAGE
                        or HardwareBuffer.USAGE_CPU_READ_RARELY
                        or HardwareBuffer.USAGE_CPU_WRITE_RARELY
            )
        }
    }

    // ---- nonroot (MediaProjection) state ----
    @Volatile private var imageReader: ImageReader? = null
    @Volatile private var virtualDisplay: VirtualDisplay? = null

    // ---- stats ----
    @Volatile private var frames = 0
    @Volatile private var latencySum = 0L
    @Volatile private var lastStatsTime = 0L
    @Volatile private var consecutiveErrors = 0

    private fun nextRing(): HardwareBuffer {
        val buf = ring[ringIdx % ringSize]
        ringIdx = (ringIdx + 1) % ringSize
        return buf
    }

    fun start() {
        synchronized(lock) {
            if (running) return
            running = true
            frames = 0
            latencySum = 0
            lastError = null
            consecutiveErrors = 0
            lastStatsTime = System.currentTimeMillis()

            // Set up nonroot MediaProjection capture if requested.
            if (mode == "nonroot") {
                setupNonroot(projection)
            }

            worker = thread(name = "lsfg-capture") {
                while (running) {
                    val t0 = System.currentTimeMillis()
                    try {
                        val ahb = captureOnce()
                        if (ahb != null) {
                            val ahbPtr = WsiBridge.nativeAhbPtr(ahb)
                            val sink = frameSink
                            val produced = if (sink != null) {
                                try {
                                    sink(ahb, ahbPtr)
                                } catch (t: Throwable) {
                                    lastError = t.message ?: "framegen failed"
                                    0
                                }
                            } else 0

                            if (produced <= 0 && ahbPtr != 0L) {
                                // Passthrough (framegen off / unavailable): present the source.
                                WsiBridge.nativePushFrame(ahbPtr)
                            }
                            if (produced > 0) {
                                frames++
                                latencySum += System.currentTimeMillis() - t0
                                consecutiveErrors = 0
                            } else {
                                frames++
                                latencySum += System.currentTimeMillis() - t0
                                consecutiveErrors = 0
                            }
                        } else {
                            // No new frame available (e.g. MediaProjection idle) -> pace and retry.
                            if (mode == "nonroot") {
                                Thread.sleep(4)
                            }
                        }
                    } catch (t: Throwable) {
                        lastError = t.message ?: "capture failed"
                        consecutiveErrors++
                        if (consecutiveErrors >= 20) {
                            onStats(0f, 0f, "capture failing repeatedly: ${lastError}")
                            consecutiveErrors = 0
                        }
                    }

                    // Pacing.
                    val elapsed = System.currentTimeMillis() - t0
                    val sleepMs = minIntervalMs - elapsed
                    if (sleepMs > 0 && running) {
                        try { Thread.sleep(sleepMs) } catch (_: InterruptedException) { break }
                    }

                    // Stats every second.
                    val now = System.currentTimeMillis()
                    if (now - lastStatsTime >= 1000L) {
                        val dt = (now - lastStatsTime) / 1000f
                        onStats(frames / dt, if (frames > 0) latencySum / frames.toFloat() else 0f, lastError)
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
        try { worker?.join(800) } catch (_: Throwable) {}
        worker = null
        // Release nonroot resources.
        try { virtualDisplay?.release() } catch (_: Throwable) {}
        virtualDisplay = null
        try { imageReader?.close() } catch (_: Throwable) {}
        imageReader = null
        // Clean up root temp file.
        try { ShellUtils.fastCmd("rm -f $pngPath") } catch (_: Throwable) {}
    }

    /** Set up MediaProjection -> VirtualDisplay -> ImageReader (nonroot). */
    private fun setupNonroot(proj: MediaProjection?) {
        if (proj == null) {
            lastError = "nonroot mode requested but no MediaProjection available"
            return
        }
        val dpi = if (densityDpi > 0) densityDpi else 320
        val reader = ImageReader.newInstance(width, height, PixelFormat.RGBA_8888, 4)
        imageReader = reader
        val vd = proj.createVirtualDisplay(
            "lsfg-capture", width, height, dpi,
            android.hardware.display.DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR,
            reader.surface, null, null
        )
        virtualDisplay = vd
        lastFrameSource = "mediaprojection"
    }

    /** Capture one frame into the next ring slot. Returns the filled AHB or null. */
    private fun captureOnce(): HardwareBuffer? {
        return when (mode) {
            "nonroot" -> captureNonroot()
            "shizuku" -> captureShell()
            else -> captureShell()
        }
    }

    // ---- nonroot: MediaProjection ImageReader -> ring (fast CPU fill) ----
    private fun captureNonroot(): HardwareBuffer? {
        val reader = imageReader ?: run {
            lastError = "nonroot: ImageReader not initialized (grant screen capture)"
            return null
        }
        val image = reader.acquireLatestImage() ?: return null
        try {
            val plane = image.planes[0]
            val buf: ByteBuffer = plane.buffer
            val n = buf.remaining()
            if (n < width * height * 4) return null
            val bytes = ByteArray(n)
            buf.duplicate().get(bytes)
            val ahb = nextRing()
            val ptr = WsiBridge.fillAhb(ahb, bytes, plane.rowStride)
            if (ptr == 0L) {
                lastError = "nonroot: fillAhb failed"
                return null
            }
            lastFrameSource = "mediaprojection"
            return ahb
        } finally {
            image.close()
        }
    }

    // ---- root / shizuku: screencap -> decode -> ring (via Canvas) ----
    private fun captureShell(): HardwareBuffer? {
        val result = Shell.cmd("screencap -p $pngPath").exec()
        if (!result.isSuccess) {
            lastError = "screencap failed (code ${result.code}): ${result.err}"
            return null
        }
        val png = readFileB64()
        if (png == null) {
            lastError = "screencap read-back failed"
            return null
        }
        val src = BitmapFactory.decodeByteArray(png, 0, png.size)
        if (src == null) {
            lastError = "png decode failed"
            return null
        }
        val fitted: Bitmap = if (src.width != width || src.height != height)
            Bitmap.createScaledBitmap(src, width, height, true) ?: src
        else src
        if (fitted !== src) src.recycle()

        val ahb = nextRing()
        val hw = Bitmap.wrapHardwareBuffer(ahb, ColorSpace.get(ColorSpace.Named.SRGB))
        if (hw == null) {
            fitted.recycle()
            lastError = "wrapHardwareBuffer failed"
            return null
        }
        Canvas(hw).drawBitmap(fitted, 0f, 0f, null)
        hw.recycle()
        fitted.recycle()
        lastFrameSource = if (mode == "shizuku") "shizuku" else "root"
        return ahb
    }

    private fun readFileB64(): ByteArray? {
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

    fun release() {
        stop()
        ring.forEach { try { it.close() } catch (_: Throwable) {} }
    }
}
