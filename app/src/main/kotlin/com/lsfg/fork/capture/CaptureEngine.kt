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
import com.lsfg.fork.Log
import com.lsfg.fork.wsi.WsiBridge
import com.topjohnwu.superuser.Shell
import com.topjohnwu.superuser.ShellUtils
import java.io.File
import java.nio.ByteBuffer
import kotlin.concurrent.thread

/**
 * Screen capture engine with three modes:
 *  - "root":     root `screencap` -> PNG file in the app's private dir ->
 *                direct File read (fast, no base64 over the shell pipe).
 *  - "shizuku":  Shizuku/shell `screencap` -> /data/local/tmp -> base64 read
 *                (shell uid cannot write into the app's private dir).
 *  - "nonroot":  MediaProjection + VirtualDisplay + ImageReader (scrcpy-style).
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
    private val tmpPath = "/data/local/tmp/lsfg_cap.png"   // shizuku path
    private val rootFile: File = File(appContext.filesDir, "lsfg_cap.png") // root path

    // ---- fixed capture ring (shared by all modes) ----
    private val ringSize = 6
    private val ring: Array<HardwareBuffer>
    private var ringIdx = 0
    init {
        ring = Array(ringSize) {
            HardwareBuffer.create(
                width, height, PixelFormat.RGBA_8888, 1,
                HardwareBuffer.USAGE_GPU_SAMPLED_IMAGE or
                        HardwareBuffer.USAGE_CPU_READ_RARELY or
                        HardwareBuffer.USAGE_CPU_WRITE_RARELY
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
    @Volatile private var lastFrameMs = 0L

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

            if (mode == "nonroot") {
                setupNonroot(projection)
            }
            Log.i("CaptureEngine", "start mode=$mode ${width}x${height} interval=${minIntervalMs}ms")

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
                                    Log.w("CaptureEngine", "frameSink: $lastError")
                                    0
                                }
                            } else 0

                            if (produced <= 0 && ahbPtr != 0L) {
                                // Passthrough (framegen off / unavailable): present source.
                                WsiBridge.nativePushFrame(ahbPtr)
                            }
                            frames++
                            latencySum += System.currentTimeMillis() - t0
                            consecutiveErrors = 0
                        } else {
                            if (mode == "nonroot") {
                                Thread.sleep(4)
                            }
                        }
                    } catch (t: Throwable) {
                        lastError = t.message ?: "capture failed"
                        consecutiveErrors++
                        if (consecutiveErrors in listOf(1, 5, 20) || consecutiveErrors % 25 == 0) {
                            Log.w("CaptureEngine", "capture error #$consecutiveErrors: $lastError")
                        }
                        if (consecutiveErrors >= 25) {
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
                        val fps = frames / dt
                        val avgLat = if (frames > 0) latencySum / frames.toFloat() else 0f
                        if (fps > 0f) Log.i("CaptureEngine", String.format("%.1f fps cap avg %.0fms src=%s", fps, avgLat, lastFrameSource))
                        onStats(fps, avgLat, lastError)
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
        Log.i("CaptureEngine", "stop")
        try { worker?.interrupt() } catch (_: Throwable) {}
        try { worker?.join(800) } catch (_: Throwable) {}
        worker = null
        try { virtualDisplay?.release() } catch (_: Throwable) {}
        virtualDisplay = null
        try { imageReader?.close() } catch (_: Throwable) {}
        imageReader = null
        try { ShellUtils.fastCmd("rm -f $tmpPath") } catch (_: Throwable) {}
        try { if (rootFile.exists()) rootFile.delete() } catch (_: Throwable) {}
    }

    /** Set up MediaProjection -> VirtualDisplay -> ImageReader (nonroot). */
    private fun setupNonroot(proj: MediaProjection?) {
        if (proj == null) {
            lastError = "nonroot mode requested but no MediaProjection available"
            Log.w("CaptureEngine", lastError!!)
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
        Log.i("CaptureEngine", "nonroot VirtualDisplay ${width}x${height}@${dpi}dpi")
    }

    /** Capture one frame into the next ring slot. Returns the filled AHB or null. */
    private fun captureOnce(): HardwareBuffer? {
        return when (mode) {
            "nonroot" -> captureNonroot()
            "shizuku" -> captureShizuku()
            else -> captureRoot()
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

    // ---- root: screencap -> app filesDir -> direct File read (fast) ----
    private fun captureRoot(): HardwareBuffer? {
        // screencap writes a PNG into the app's private files dir. Root can
        // write there even though the dir is app-owned; the app then reads the
        // file directly (no base64 transfer over the shell pipe).
        val res = Shell.cmd("screencap -p ${rootFile.absolutePath}").exec()
        if (!res.isSuccess) {
            lastError = "screencap failed (code ${res.code}): ${res.err}"
            return null
        }
        val src = BitmapFactory.decodeFile(rootFile.absolutePath)
        try { if (rootFile.exists()) rootFile.delete() } catch (_: Throwable) {}
        if (src == null) {
            lastError = "png decode failed (root)"
            return null
        }
        return fillRing(src)
    }

    // ---- shizuku: screencap -> /data/local/tmp -> base64 read ----
    private fun captureShizuku(): HardwareBuffer? {
        val res = Shell.cmd("screencap -p $tmpPath").exec()
        if (!res.isSuccess) {
            lastError = "screencap failed (code ${res.code}): ${res.err}"
            return null
        }
        val png = readFileB64()
        try { ShellUtils.fastCmd("rm -f $tmpPath") } catch (_: Throwable) {}
        if (png == null) {
            lastError = "screencap read-back failed (shizuku)"
            return null
        }
        val src = BitmapFactory.decodeByteArray(png, 0, png.size)
        if (src == null) {
            lastError = "png decode failed (shizuku)"
            return null
        }
        return fillRing(src)
    }

    /** Scale (if needed) and blit a decoded source bitmap into the next ring slot. */
    private fun fillRing(src: Bitmap): HardwareBuffer? {
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
        val out = Shell.cmd("base64 -w 0 $tmpPath").exec()
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
