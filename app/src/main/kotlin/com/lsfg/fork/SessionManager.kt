package com.lsfg.fork

import android.content.Context
import android.graphics.PixelFormat
import android.hardware.HardwareBuffer
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.lsfg.fork.capture.CaptureEngine
import com.lsfg.fork.framegen.FramegenBridge
import com.lsfg.fork.wsi.WsiBridge
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/**
 * Minimal session orchestrator.
 *
 * Path: root screencap -> HardwareBuffer -> WSI presenter (overlay surface).
 * Framegen is wired but bypassed by default (passthrough) so the app runs
 * even before the compute pipeline is verified on-device.
 */
class SessionManager : ViewModel() {

    companion object {
        /** Process-wide shared instance (main UI + overlay activity). */
        @Volatile
        var shared: SessionManager = SessionManager()
    }

    data class State(
        val running: Boolean = false,
        val mode: String = "root",
        val rootInfo: RootChecker.RootState = RootChecker.RootState(false, "none", false, -1),
        val fps: Float = 0f,
        val captureLatencyMs: Float = 0f,
        val targetPkg: String? = null,
        val multiplier: Int = 2,
        val flowScale: Float = 1.0f,
        val performance: Boolean = false,
        val framegenBypass: Boolean = true,
        val framegenReady: Boolean = false,
        val statusMsg: String = "idle",
        val targetFps: Float = 60f,
        val displayW: Int = 0,
        val displayH: Int = 0
    )

    private val _state = MutableStateFlow(State())
    val state: StateFlow<State> = _state.asStateFlow()

    private var engine: CaptureEngine? = null
    @Volatile private var windowPtr: Long = 0L
    @Volatile private var presenterSurface: Any? = null

    // Real frame-generation session (LSFG pipeline). false => passthrough.
    @Volatile private var framegenSessionOk: Boolean = false
    // Captured real-frame counter; its parity selects the pipeline input layer
    // (the pipeline reads layer iteration%2, which stays in lockstep with this).
    @Volatile private var capturedFrameIdx: Int = 0
    // Rotating pool of destination AHBs for generated frames (so a buffer the
    // presenter has not yet read is never overwritten by the next generated frame).
    private val dstPool = java.util.ArrayList<HardwareBuffer>()
    @Volatile private var dstPoolIdx: Int = 0
    @Volatile private var displayW: Int = 0
    @Volatile private var displayH: Int = 0

    init {
        viewModelScope.launch(Dispatchers.IO) {
            val r = RootChecker.detect()
            withContext(Dispatchers.Main) { _state.update { it.copy(rootInfo = r) } }
        }
    }

    fun setTargetPkg(pkg: String?) = _state.update { it.copy(targetPkg = pkg) }
    fun setMultiplier(v: Int) = _state.update { it.copy(multiplier = v) }
    fun setFlowScale(v: Float) = _state.update { it.copy(flowScale = v) }
    fun setPerformance(v: Boolean) = _state.update { it.copy(performance = v) }
    fun setFramegenBypass(v: Boolean) = _state.update { it.copy(framegenBypass = v) }
    fun setTargetFps(v: Float) = _state.update { it.copy(targetFps = v) }
    fun setDisplaySize(w: Int, h: Int) = _state.update { it.copy(displayW = w, displayH = h) }

    fun refreshRoot() {
        viewModelScope.launch(Dispatchers.IO) {
            val r = RootChecker.detect()
            withContext(Dispatchers.Main) { _state.update { it.copy(rootInfo = r) } }
        }
    }

    /**
     * Called by OverlayActivity with the native window pointer of the
     * overlay SurfaceView. Starts the presenter + capture engine, and (unless
     * bypassed) sets up the real LSFG frame-generation session.
     */
    fun beginSession(nativeWindow: Long, surface: Any, w: Int, h: Int, appContext: Context) {
        if (_state.value.running) return
        val s = _state.value
        this.windowPtr = nativeWindow
        this.presenterSurface = surface
        this.displayW = w
        this.displayH = h

        WsiBridge.start(nativeWindow, w, h, s.targetFps)
        _state.update { it.copy(running = true, statusMsg = "presenter up", displayW = w, displayH = h) }

        framegenSessionOk = false
        capturedFrameIdx = 0
        dstPoolIdx = 0
        dstPool.clear()

        if (!s.framegenBypass) {
            val ok = setupFramegen(w, h, s, appContext)
            framegenSessionOk = ok
            _state.update {
                it.copy(
                    framegenReady = ok,
                    statusMsg = if (ok) "framegen on (LSFG v3.1)" else "framegen unavailable, passthrough"
                )
            }
        } else {
            _state.update { it.copy(framegenReady = false) }
        }

        // Pre-allocate the destination AHB pool for generated frames.
        val poolSize = 8
        repeat(poolSize) {
            dstPool.add(
                HardwareBuffer.create(
                    w, h, PixelFormat.RGBA_8888, 1,
                    HardwareBuffer.USAGE_GPU_SAMPLED_IMAGE
                )
            )
        }

        val eng = CaptureEngine(
            width = w,
            height = h,
            minIntervalMs = (1000f / 30f).toLong(),
            onStats = { fps, lat -> _state.update { it.copy(fps = fps, captureLatencyMs = lat) } }
        )
        if (framegenSessionOk) {
            eng.frameSink = { srcAhb, srcAhbPtr -> driveFramegen(srcAhb, srcAhbPtr, s.multiplier) }
        }
        engine = eng
        eng.start()
    }

    /**
     * Drive the real frame-generation pipeline for one captured source frame:
     * write the source, dispatch the pre-pass, then acquire `multiplier`
     * generated frames into fresh destination AHBs (pushed to the presenter).
     * Returns the number of frames produced (0 => caller falls back to passthrough).
     */
    private fun driveFramegen(srcAhb: HardwareBuffer, srcAhbPtr: Long, multiplier: Int): Int {
        if (!framegenSessionOk) return 0
        val idx = capturedFrameIdx
        val m = if (multiplier < 1) 1 else multiplier

        // Write the captured source frame into the pipeline input image.
        FramegenBridge.writeSource(srcAhbPtr, idx)

        // Pre-pass (flow estimation + temporals) for `m` output frames.
        FramegenBridge.dispatch(m)

        var produced = 0
        repeat(m) {
            val dst = dstPool[dstPoolIdx % dstPool.size]
            val dstPtr = WsiBridge.nativeAhbPtr(dst)
            if (dstPtr == 0L) return@repeat
            FramegenBridge.acquireToDst(dstPtr)
            WsiBridge.nativePushFrame(dstPtr)
            dstPoolIdx++
            produced++
        }
        capturedFrameIdx = idx + 1
        if (produced == 0) {
            _state.update { it.copy(statusMsg = "framegen produced 0, passthrough") }
        }
        return produced
    }

    /**
     * Initialize the LSFG frame-generation session from the bundled shader DLL.
     * Copies the DLL asset into app files, then creates the Vulkan context.
     */
    private fun setupFramegen(w: Int, h: Int, s: State, ctx: Context): Boolean {
        return try {
            val dllPath = extractShaderDll(ctx)
            val okInit = FramegenBridge.init(dllPath, allowFP16 = s.performance)
            if (!okInit) {
                _state.update { it.copy(statusMsg = "framegen init failed: shaders/dll") }
                return false
            }
            val okSession = FramegenBridge.createSession(w, h, s.flowScale, s.performance)
            if (!okSession) {
                FramegenBridge.destroy()
                _state.update { it.copy(statusMsg = "framegen createSession failed") }
                return false
            }
            true
        } catch (t: Throwable) {
            _state.update { it.copy(statusMsg = "framegen setup threw: ${t.message}") }
            false
        }
    }

    private val ASSET_DLL = "lsfg-vk.dll"

    /** Copy the bundled LSFG shader DLL asset into the app's private files dir. */
    private fun extractShaderDll(ctx: Context): String {
        val app = if (ctx is android.app.Application) ctx else ctx.applicationContext
        val target = java.io.File(app.filesDir, ASSET_DLL)
        if (!target.exists() || target.length() == 0L) {
            try {
                app.assets.open(ASSET_DLL).use { input ->
                    target.outputStream().use { output -> input.copyTo(output) }
                }
            } catch (t: Throwable) {
                // Fall through; init() will fail with a clear message.
            }
        }
        return target.absolutePath
    }

    fun endSession() {
        if (!_state.value.running) return
        engine?.stop()
        engine = null
        WsiBridge.stop()
        // Tear down the frame-generation session (waits for in-flight GPU work).
        if (framegenSessionOk) {
            try { FramegenBridge.idle() } catch (_: Throwable) {}
            try { FramegenBridge.destroy() } catch (_: Throwable) {}
        }
        framegenSessionOk = false
        // Release the destination AHB pool.
        for (buf in dstPool) { try { buf.close() } catch (_: Throwable) {} }
        dstPool.clear()
        if (windowPtr != 0L) {
            WsiBridge.nativeReleaseWindow(windowPtr)
            windowPtr = 0L
        }
        presenterSurface = null
        _state.update { it.copy(running = false, fps = 0f, captureLatencyMs = 0f, statusMsg = "stopped") }
    }

    fun isRunning(): Boolean = _state.value.running

    override fun onCleared() {
        endSession()
    }
}
