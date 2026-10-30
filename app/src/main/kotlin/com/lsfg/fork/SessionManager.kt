package com.lsfg.fork

import android.content.Context
import android.graphics.PixelFormat
import android.hardware.HardwareBuffer
import android.media.projection.MediaProjection
import android.media.projection.MediaProjectionManager
import android.os.Build
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
 * Session orchestrator.
 *
 * Pipeline: CaptureEngine (nonroot MediaProjection | root/shizuku screencap)
 *   -> framegen (LSFG v3_1 Vulkan pipeline, user or bundled shader DLL)
 *   -> WSI presenter (Vulkan swapchain on the overlay surface).
 *
 * Framegen is ON by default; if the shader DLL or Vulkan session fails the
 * engine transparently falls back to passthrough (source frames presented as-is).
 */
class SessionManager : ViewModel() {

    companion object {
        /** Process-wide shared instance (main UI + service + notification). */
        @Volatile
        var shared: SessionManager = SessionManager()

        /** MediaProjection consent result data (from the system dialog),
         *  stashed by the activity that received onActivityResult. */
        @Volatile
        var pendingProjectionData: android.content.Intent? = null

        const val TAG = "lsfg-fork"
    }

    data class State(
        val running: Boolean = false,
        val captureMode: String = "root",
        val rootInfo: RootChecker.RootState = RootChecker.RootState(false, "none", false, -1),
        val fps: Float = 0f,
        val captureLatencyMs: Float = 0f,
        val targetPkg: String? = null,
        val multiplier: Int = 2,
        val flowScale: Float = 1.0f,
        val performance: Boolean = false,
        val framegenBypass: Boolean = false,
        val framegenReady: Boolean = false,
        val framegenSource: String = "bundled",
        val statusMsg: String = "idle",
        val targetFps: Float = 60f,
        val displayW: Int = 0,
        val displayH: Int = 0,
        val queueDepth: Int = 0
    )

    private val _state = MutableStateFlow(State())
    val state: StateFlow<State> = _state.asStateFlow()

    private var engine: CaptureEngine? = null
    @Volatile private var windowPtr: Long = 0L
    @Volatile private var mediaProjection: MediaProjection? = null

    // Real frame-generation session (LSFG pipeline). false => passthrough.
    @Volatile private var framegenSessionOk: Boolean = false
    @Volatile private var capturedFrameIdx: Int = 0
    private val dstPool = java.util.ArrayList<HardwareBuffer>()
    @Volatile private var dstPoolIdx: Int = 0
    @Volatile private var displayW: Int = 0
    @Volatile private var displayH: Int = 0

    init {
        val r = RootChecker.detect()
        _state.update { it.copy(rootInfo = r) }
        // Pick the best default capture mode for this device.
        val mode = if (r.root) "root" else "nonroot"
        _state.update { it.copy(captureMode = mode) }
    }

    fun setTargetPkg(pkg: String?) = _state.update { it.copy(targetPkg = pkg) }
    fun setMultiplier(v: Int) = _state.update { it.copy(multiplier = v) }
    fun setFlowScale(v: Float) = _state.update { it.copy(flowScale = v) }
    fun setPerformance(v: Boolean) = _state.update { it.copy(performance = v) }
    fun setFramegenBypass(v: Boolean) = _state.update { it.copy(framegenBypass = v) }
    fun setCaptureMode(mode: String) = _state.update { it.copy(captureMode = mode) }
    fun setTargetFps(v: Float) = _state.update { it.copy(targetFps = v) }

    fun refreshRoot() {
        viewModelScope.launch(Dispatchers.IO) {
            val r = RootChecker.detect()
            withContext(Dispatchers.Main) { _state.update { it.copy(rootInfo = r) } }
        }
    }

    /**
     * Begin the session on the overlay surface.
     *
     * @param nativeWindow ANativeWindow pointer of the overlay SurfaceView.
     * @param surface      the overlay Surface (for diagnostics).
     * @param w/h          presentation size in pixels.
     * @param appContext   used to extract the shader DLL + create MediaProjection.
     * @param userDllPath  optional user-supplied Lossless.dll / lsfg-vk.dll.
     */
    fun beginSession(
        surface: android.view.Surface,
        w: Int,
        h: Int,
        appContext: Context,
        userDllPath: String? = null
    ) {
        if (_state.value.running) return
        val s = _state.value
        this.displayW = w
        this.displayH = h

        // Acquire a fresh ANativeWindow reference for this session (balanced by
        // endSession) so live restarts never use a released window.
        val nativeWindow = WsiBridge.nativeSurfaceToWindow(surface)
        if (nativeWindow == 0L) {
            _state.update { it.copy(statusMsg = "overlay surface -> window failed") }
            return
        }
        this.windowPtr = nativeWindow

        // 1. Presenter up (Vulkan swapchain on the overlay).
        WsiBridge.start(nativeWindow, w, h, s.targetFps)
        _state.update {
            it.copy(running = true, statusMsg = "presenter up", displayW = w, displayH = h)
        }

        // 2. Framegen session (unless bypassed).
        framegenSessionOk = false
        capturedFrameIdx = 0
        dstPoolIdx = 0
        dstPool.clear()
        if (!s.framegenBypass) {
            val dllPath = userDllPath?.takeIf { it.isNotBlank() } ?: extractShaderDll(appContext)
            val srcLabel = if (userDllPath != null && userDllPath.isNotBlank()) "user" else "bundled"
            val ok = setupFramegen(w, h, s, dllPath)
            framegenSessionOk = ok
            _state.update {
                it.copy(
                    framegenReady = ok,
                    framegenSource = srcLabel,
                    statusMsg = if (ok) "framegen ON (LSFG v3.1, $srcLabel dll)"
                                else "framegen unavailable -> passthrough"
                )
            }
        } else {
            _state.update { it.copy(framegenReady = false) }
        }

        // 3. Destination AHB pool for generated frames (presenter reads these).
        val poolSize = 8
        repeat(poolSize) {
            dstPool.add(
                HardwareBuffer.create(
                    w, h, PixelFormat.RGBA_8888, 1,
                    HardwareBuffer.USAGE_GPU_SAMPLED_IMAGE
                )
            )
        }

        // 4. Capture engine (mode from state; MediaProjection for nonroot).
        val mode = s.captureMode
        var proj: MediaProjection? = null
        if (mode == "nonroot") {
            proj = createProjection(appContext)
            if (proj == null) {
                _state.update {
                    it.copy(statusMsg = "nonroot needs MediaProjection consent - use Start in the app")
                }
            }
        }
        val eng = CaptureEngine(
            appContext = appContext,
            width = w,
            height = h,
            minIntervalMs = (1000f / 30f).toLong(),
            onStats = { fps, lat, err ->
                _state.update {
                    it.copy(
                        fps = fps,
                        captureLatencyMs = lat,
                        queueDepth = WsiBridge.queueSize(),
                        statusMsg = err ?: it.statusMsg
                    )
                }
            },
            projection = proj
        )
        eng.mode = mode
        eng.frameSink = { srcAhb, srcAhbPtr -> driveFramegen(srcAhb, srcAhbPtr, s.multiplier) }
        engine = eng
        eng.start()
        mediaProjection = proj

        if (framegenSessionOk) {
            _state.update { it.copy(statusMsg = "running: ${mode} capture -> LSFG v3_1 -> overlay") }
        }
    }

    private fun createProjection(appContext: Context): MediaProjection? {
        return try {
            val data = pendingProjectionData
            if (data == null) {
                android.util.Log.w(TAG, "no MediaProjection consent data; nonroot unavailable")
                null
            } else {
                val mpm = appContext.getSystemService(MediaProjectionManager::class.java)
                mpm.getMediaProjection(Build.VERSION.SDK_INT, data)
            }
        } catch (t: Throwable) {
            android.util.Log.e(TAG, "createProjection failed", t)
            null
        }
    }

    /**
     * Drive the LSFG pipeline for one captured source frame.
     */
    private fun driveFramegen(srcAhb: HardwareBuffer, srcAhbPtr: Long, multiplier: Int): Int {
        if (!framegenSessionOk) return 0
        val idx = capturedFrameIdx
        val m = if (multiplier < 1) 1 else multiplier

        FramegenBridge.writeSource(srcAhbPtr, idx)
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
        return produced
    }

    private fun setupFramegen(w: Int, h: Int, s: State, dllPath: String): Boolean {
        return try {
            val okInit = FramegenBridge.init(dllPath, allowFP16 = s.performance)
            if (!okInit) {
                _state.update { it.copy(statusMsg = "framegen init failed: shader DLL ($dllPath)") }
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
                android.util.Log.e(TAG, "extractShaderDll failed: ${t.message}")
            }
        }
        return target.absolutePath
    }

    fun endSession() {
        if (!_state.value.running) return
        engine?.release()
        engine = null
        WsiBridge.stop()
        if (framegenSessionOk) {
            try { FramegenBridge.idle() } catch (_: Throwable) {}
            try { FramegenBridge.destroy() } catch (_: Throwable) {}
        }
        framegenSessionOk = false
        for (buf in dstPool) { try { buf.close() } catch (_: Throwable) {} }
        dstPool.clear()
        try { mediaProjection?.stop() } catch (_: Throwable) {}
        mediaProjection = null
        if (windowPtr != 0L) {
            WsiBridge.nativeReleaseWindow(windowPtr)
            windowPtr = 0L
        }
        _state.update {
            it.copy(running = false, fps = 0f, captureLatencyMs = 0f, queueDepth = 0, statusMsg = "stopped")
        }
    }

    fun isRunning(): Boolean = _state.value.running

    override fun onCleared() {
        endSession()
    }
}
