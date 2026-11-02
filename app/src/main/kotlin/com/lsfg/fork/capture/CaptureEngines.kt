package com.lsfg.fork.capture

import android.content.Context
import com.lsfg.fork.Log
import com.lsfg.fork.session.RootCaptureEngine
import com.lsfg.fork.session.ShizukuCaptureEngine
import com.lsfg.fork.wsi.WsiBridge

/**
 * Thin drivers for the ported privileged-capture engines
 * (com.lsfg.fork.session.RootCaptureEngine / ShizukuCaptureEngine, stolen from
 * the LSFG-Android reference session/ package). The engines receive
 * HardwareBuffer frames over AIDL and push them into NativeBridge, which is
 * wired to the session's framegen/passthrough sink by SessionManager.
 *
 * These adapters only manage the engine lifecycle and report session stats
 * (posted-frame deltas from the WSI presenter, the ground-truth counter).
 */
class RootCaptureEngineAdapter(private val ctx: Context) {

    @Volatile
    var onError: ((String) -> Unit)? = null

    @Volatile
    var onStats: ((framesPerSec: Float, avgLatencyMs: Float, err: String?) -> Unit)? = null

    private var engine: RootCaptureEngine? = null
    @Volatile
    var running: Boolean = false
        private set
    @Volatile
    var lastError: String? = null

    private var statsThread: Thread? = null

    fun isReady(): Boolean = runCatching { RootCaptureEngine(ctx).isReady() }.getOrDefault(false)

    fun startCapture(targetPackage: String, width: Int, height: Int, maxFps: Int) {
        if (running) stop()
        val eng = RootCaptureEngine(ctx)
        eng.setErrorListener { msg ->
            lastError = msg
            Log.w(TAG, "root capture: $msg")
            onError?.invoke(msg)
        }
        engine = eng
        lastError = null
        running = true
        eng.startCapture(targetPackage, width, height, maxFps)
        Log.i(TAG, "root capture started $targetPackage ${width}x${height} maxFps=$maxFps")
        statsThread = Thread({ statsLoop() }, "lsfg-root-adapter").also { it.start() }
    }

    fun stop() {
        if (!running) return
        running = false
        runCatching { engine?.stop() }
        engine = null
        try { statsThread?.interrupt() } catch (_: Throwable) {}
        statsThread = null
        Log.i(TAG, "root capture stopped")
    }

    private fun statsLoop() {
        var lastPosted: Long = 0L
        var lastTime: Long = System.currentTimeMillis()
        while (running) {
            try { Thread.sleep(1000L) } catch (_: InterruptedException) { break }
            val now = System.currentTimeMillis()
            val dt = (now - lastTime) / 1000f
            if (dt >= 1f) {
                val posted = try { WsiBridge.framesPresented() } catch (_: Throwable) { 0L }
                val fps = ((posted - lastPosted) * 1000L / (now - lastTime)).toFloat()
                lastPosted = posted
                lastTime = now
                Log.i(TAG, String.format("root session %.1f posted-fps (total %d)", fps, posted))
                onStats?.invoke(fps, 0f, lastError)
            }
        }
    }

    fun release() {
        stop()
    }

    companion object {
        private const val TAG = "RootCaptureAdapter"
    }
}

/** Driver for the Shizuku privileged ScreenCapture engine. */
class ShizukuCaptureEngineAdapter(private val ctx: Context) {

    @Volatile
    var onError: ((String) -> Unit)? = null

    @Volatile
    var onStats: ((framesPerSec: Float, avgLatencyMs: Float, err: String?) -> Unit)? = null

    private var engine: ShizukuCaptureEngine? = null
    @Volatile
    var running: Boolean = false
        private set
    @Volatile
    var lastError: String? = null

    private var statsThread: Thread? = null

    fun isReady(): Boolean = runCatching { ShizukuCaptureEngine(ctx).isReady() }.getOrDefault(false)

    fun startCapture(targetPackage: String, width: Int, height: Int, maxFps: Int) {
        if (running) stop()
        val eng = ShizukuCaptureEngine(ctx)
        eng.setErrorListener { msg ->
            lastError = msg
            Log.w(TAG, "shizuku capture: $msg")
            onError?.invoke(msg)
        }
        engine = eng
        lastError = null
        running = true
        eng.startCapture(targetPackage, width, height, maxFps)
        Log.i(TAG, "shizuku capture started $targetPackage ${width}x${height} maxFps=$maxFps")
        statsThread = Thread({ statsLoop() }, "lsfg-shizuku-adapter").also { it.start() }
    }

    fun stop() {
        if (!running) return
        running = false
        runCatching { engine?.stop() }
        engine = null
        try { statsThread?.interrupt() } catch (_: Throwable) {}
        statsThread = null
        Log.i(TAG, "shizuku capture stopped")
    }

    private fun statsLoop() {
        var lastPosted: Long = 0L
        var lastTime: Long = System.currentTimeMillis()
        while (running) {
            try { Thread.sleep(1000L) } catch (_: InterruptedException) { break }
            val now = System.currentTimeMillis()
            val dt = (now - lastTime) / 1000f
            if (dt >= 1f) {
                val posted = try { WsiBridge.framesPresented() } catch (_: Throwable) { 0L }
                val fps = ((posted - lastPosted) * 1000L / (now - lastTime)).toFloat()
                lastPosted = posted
                lastTime = now
                Log.i(TAG, String.format("shizuku session %.1f posted-fps (total %d)", fps, posted))
                onStats?.invoke(fps, 0f, lastError)
            }
        }
    }

    fun release() {
        stop()
    }

    companion object {
        private const val TAG = "ShizukuCaptureAdapter"
    }
}
