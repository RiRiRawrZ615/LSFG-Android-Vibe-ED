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
import com.lsfg.fork.testpattern.TestPattern
import com.lsfg.fork.wsi.WsiBridge
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlin.concurrent.thread

/**
 * Session orchestrator.
 *
 * Pipeline:  frame source (TestPattern | CaptureEngine)
 *          -> framegen (LSFG v3_1 Vulkan pipeline, user or bundled shader DLL)
 *          -> WSI presenter (Vulkan swapchain on the overlay surface).
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
        val queueDepth: Int = 0,
        val testPattern: Boolean = false,
        val lastCaptureProbe: RootChecker.ProbeResult? = null,
        // Honest pipeline telemetry (fixes "claims pass when it fails"):
        val framesPresented: Long = 0,
        val pipelineOk: Boolean = false
    )

    private val _state = MutableStateFlow(State())
    val state: StateFlow<State> = _state.asStateFlow()

    private var engine: CaptureEngine? = null
    private var testPattern: TestPattern? = null
    private var testThread: Thread? = null
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
        // Default capture mode: root if available, else nonroot.
        val mode = if (r.root) "root" else "nonroot"
        _state.update { it.copy(captureMode = mode) }
        Log.i(TAG, "SessionManager init: root=${r.root} provider=${r.provider} defaultMode=$mode")
    }

    fun setTargetPkg(pkg: String?) = _state.update { it.copy(targetPkg = pkg) }
    fun setMultiplier(v: Int) = _state.update { it.copy(multiplier = v) }
    fun setFlowScale(v: Float) = _state.update { it.copy(flowScale = v) }
    fun setPerformance(v: Boolean) = _state.update { it.copy(performance = v) }
    fun setFramegenBypass(v: Boolean) = _state.update { it.copy(framegenBypass = v) }
    fun setCaptureMode(mode: String) = _state.update { it.copy(captureMode = mode) }
    fun setTargetFps(v: Float) = _state.update { it.copy(targetFps = v) }
    fun setTestPattern(v: Boolean) = _state.update { it.copy(testPattern = v) }

    fun refreshRoot() {
        viewModelScope.launch(Dispatchers.IO) {
            val r = RootChecker.detect()
            withContext(Dispatchers.Main) { _state.update { it.copy(rootInfo = r) } }
            Log.i(TAG, "refreshRoot: root=${r.root} provider=${r.provider}")
        }
    }

    /**
     * Run a real root capture round-trip probe (bug 2 / bug 6). Returns the
     * result synchronously on the caller thread (call from a background thread
     * or viewModelScope).
     */
    fun runCaptureProbe(appContext: Context) {
        viewModelScope.launch(Dispatchers.IO) {
            Log.i(TAG, "running capture probe (mode=${_state.value.captureMode})")
            val probe = RootChecker.testCapture(appContext)
            Log.i(TAG, "capture probe: ok=${probe.ok} detail=${probe.detail}")
            withContext(Dispatchers.Main) {
                _state.update { it.copy(lastCaptureProbe = probe) }
            }
        }
    }

    /**
     * Begin the session on the overlay surface.
     *
     * @param surface the overlay Surface (Vulkan WSI target).
     * @param w/h     presentation size in pixels.
     * @param appContext used for shader DLL extraction + MediaProjection.
     * @param userDllPath optional user-supplied shader DLL.
     */
    fun beginSession(
        surface: android.view.Surface,
        w: Int,
        h: Int,
        appContext: Context,
        userDllPath: String? = null
    ) {
        if (_state.value.running) {
            Log.w(TAG, "beginSession: already running, ignored")
            return
        }
        val s = _state.value
        this.displayW = w
        this.displayH = h

        Log.i(TAG, "beginSession ${w}x${h} mode=${s.captureMode} test=${s.testPattern} " +
                "framegen=${if (s.framegenBypass) "off" else "on"} mult=${s.multiplier} " +
                "flow=${s.flowScale} perf=${s.performance} dll=${userDllPath ?: "bundled"}")

        // If the surface isn't ready yet (attach added the window on main but
        // surfaceCreated hasn't fired), wait for it HERE on this background
        // thread. Never block the main thread (that deadlocks surfaceCreated).
        var activeSurface: android.view.Surface = surface
        if (!activeSurface.isValid) {
            Log.w(TAG, "beginSession: surface not valid yet, waiting (bg thread)")
            val waited = com.lsfg.fork.overlay.OverlayHost.awaitSurface(5000L)
            if (waited != null && waited.isValid) {
                activeSurface = waited
            }
        }
        if (!activeSurface.isValid) {
            Log.e(TAG, "beginSession: overlay surface never became ready")
            _state.update { it.copy(statusMsg = "overlay surface never became ready") }
            return
        }

        // Acquire a fresh ANativeWindow reference for this session (balanced by endSession).
        val nativeWindow = WsiBridge.nativeSurfaceToWindow(activeSurface)
        if (nativeWindow == 0L) {
            Log.e(TAG, "beginSession: surface -> ANativeWindow failed")
            _state.update { it.copy(statusMsg = "overlay surface -> window failed") }
            return
        }
        this.windowPtr = nativeWindow

        // 1. Presenter up (Vulkan swapchain on the overlay).
        val wsiOk = try {
            WsiBridge.start(nativeWindow, w, h, s.targetFps)
            WsiBridge.isRunning()
        } catch (t: Throwable) {
            Log.e(TAG, "beginSession: WSI start threw: ${t.message}")
            false
        }
        if (!wsiOk) {
            Log.e(TAG, "beginSession: WSI presenter is NOT running; aborting")
            _state.update { it.copy(statusMsg = "WSI presenter failed to start") }
            WsiBridge.nativeReleaseWindow(nativeWindow)
            this.windowPtr = 0L
            return
        }
        _state.update { it.copy(running = true, statusMsg = "presenter up", displayW = w, displayH = h) }
        Log.i(TAG, "beginSession: WSI presenter up, targetFps=${s.targetFps}")

        // 2. Framegen session (unless bypassed).
        framegenSessionOk = false
        capturedFrameIdx = 0
        dstPoolIdx = 0
        dstPool.clear()
        if (!s.framegenBypass) {
            val dllPath = userDllPath?.takeIf { it.isNotBlank() } ?: extractShaderDll(appContext)
            val srcLabel = if (userDllPath != null && userDllPath.isNotBlank()) "user" else "bundled"
            Log.i(TAG, "beginSession: setting up framegen from $dllPath ($srcLabel)")
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
            Log.i(TAG, "beginSession: framegen ${if (ok) "READY" else "NOT ready -> passthrough"}")
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

        // 4. Frame source: built-in test pattern OR capture engine.
        val onStats = { fps: Float, lat: Float, err: String? ->
            // Honest health check: the session is only "OK" if the presenter
            // has actually presented at least one frame (Vulkan path live).
            val presented = try { WsiBridge.framesPresented() } catch (_: Throwable) { 0L }
            val healthy = presented > 0L && err == null
            val status = if (err != null) err
                else if (presented == 0L) "running BUT presenter produced 0 frames (check logcat lsfg-wsi)"
                else "running: ${s.captureMode} -> LSFG -> overlay"
            _state.update {
                it.copy(
                    fps = fps,
                    captureLatencyMs = lat,
                    queueDepth = try { WsiBridge.queueSize() } catch (_: Throwable) { 0 },
                    framesPresented = presented,
                    pipelineOk = healthy,
                    statusMsg = status
                )
            }
        }
        val frameSink: (HardwareBuffer, Long) -> Int = { srcAhb, srcAhbPtr ->
            driveFramegen(srcAhb, srcAhbPtr, s.multiplier)
        }

        if (s.testPattern) {
            val pattern = TestPattern(w, h)
            testPattern = pattern
            // Feed the pattern at 30 fps (canonical framegen test source).
            testThread = thread(name = "lsfg-testpattern", isDaemon = true) {
                val intervalNs = 1_000_000_000L / 30
                var ok = 0
                while (_state.value.running) {
                    // keep producing while the session is live
                    if (!_state.value.running) break
                    val t0 = System.nanoTime()
                    try {
                        val ahb = pattern.next()
                        if (ahb != null) {
                            val ptr = WsiBridge.nativeAhbPtr(ahb)
                            if (framegenSessionOk) {
                                val produced = driveFramegen(ahb, ptr, _state.value.multiplier)
                                if (produced > 0) ok++
                            } else if (ptr != 0L) {
                                WsiBridge.nativePushFrame(ptr)
                                ok++
                            }
                        }
                    } catch (t: Throwable) {
                        Log.w(TAG, "testpattern frame failed: ${t.message}")
                    }
                    // Pace to ~30 fps.
                    val elapsed = System.nanoTime() - t0
                    val sleep = intervalNs - elapsed
                    if (sleep > 0) {
                        val sNanos = (sleep / 1_000_000_000L).coerceAtLeast(0)
                        val sNano = (sleep % 1_000_000_000L)
                        if (sNanos > 0) Thread.sleep(sNanos)
                        if (sNano > 0) Thread.sleep(0, sNano.toInt())
                    }
                }
                Log.i(TAG, "testpattern stopped after $ok frames")
            }
            Log.i(TAG, "beginSession: test pattern source started (30 fps)")
        } else {
            val mode = s.captureMode
            var proj: MediaProjection? = null
            if (mode == "nonroot") {
                proj = createProjection(appContext)
                if (proj == null) {
                    Log.e(TAG, "beginSession: nonroot but no MediaProjection consent")
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
                onStats = onStats,
                projection = proj
            )
            eng.mode = mode
            eng.frameSink = frameSink
            engine = eng
            eng.start()
            mediaProjection = proj
            Log.i(TAG, "beginSession: capture engine started (mode=$mode)")
        }

        if (framegenSessionOk) {
            _state.update { it.copy(statusMsg = "running: ${s.captureMode} -> LSFG v3.1 -> overlay") }
            Log.i(TAG, "beginSession: RUNNING (framegen path active)")
        } else {
            Log.i(TAG, "beginSession: RUNNING (passthrough path)")
        }
    }

    private fun createProjection(appContext: Context): MediaProjection? {
        return try {
            val data = pendingProjectionData
            if (data == null) {
                Log.w(TAG, "no MediaProjection consent data; nonroot unavailable")
                null
            } else {
                val mpm = appContext.getSystemService(MediaProjectionManager::class.java)
                val mp = mpm.getMediaProjection(Build.VERSION.SDK_INT, data)
                Log.i(TAG, "MediaProjection created (nonroot)")
                mp
            }
        } catch (t: Throwable) {
            Log.e(TAG, "createProjection failed: ${t.message}")
            null
        }
    }

    /**
     * Drive the LSFG pipeline for one captured source frame.
     */
    private fun driveFramegen(srcAhb: HardwareBuffer, srcAhbPtr: Long, multiplier: Int): Int {
        if (!framegenSessionOk) return 0
        // The framegen pipeline's external input is a 2-layer image (prev/cur).
        // The slot must stay in [0,1]; the unbounded counter would eventually
        // write to a non-existent layer (native OOB -> the ~5s crash).
        val slot = (capturedFrameIdx % 2)
        val m = if (multiplier < 1) 1 else multiplier

        FramegenBridge.writeSource(srcAhbPtr, slot)
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
        capturedFrameIdx++
        return produced
    }

    private fun setupFramegen(w: Int, h: Int, s: State, dllPath: String): Boolean {
        return try {
            val okInit = FramegenBridge.init(dllPath, allowFP16 = s.performance)
            if (!okInit) {
                Log.e(TAG, "framegen init FAILED for $dllPath")
                _state.update { it.copy(statusMsg = "framegen init failed: shader DLL ($dllPath)") }
                return false
            }
            val okSession = FramegenBridge.createSession(w, h, s.flowScale, s.performance)
            if (!okSession) {
                FramegenBridge.destroy()
                Log.e(TAG, "framegen createSession FAILED ${w}x${h} flow=${s.flowScale}")
                _state.update { it.copy(statusMsg = "framegen createSession failed") }
                return false
            }
            Log.i(TAG, "framegen session created ${w}x${h} flow=${s.flowScale} perf=${s.performance}")
            true
        } catch (t: Throwable) {
            Log.e(TAG, "framegen setup threw: ${t.message}")
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
                Log.i(TAG, "extracted bundled shader DLL -> ${target.absolutePath} (${target.length()}B)")
            } catch (t: Throwable) {
                Log.e(TAG, "extractShaderDll failed: ${t.message}")
            }
        }
        return target.absolutePath
    }

    fun endSession() {
        if (!_state.value.running) return
        Log.i(TAG, "endSession")
        try { testThread?.interrupt() } catch (_: Throwable) {}
        testThread = null
        try { testPattern?.stop() } catch (_: Throwable) {}
        testPattern = null
        try { engine?.release() } catch (t: Throwable) { Log.w(TAG, "engine release: ${t.message}") }
        engine = null
        try { WsiBridge.stop() } catch (t: Throwable) { Log.w(TAG, "wsi stop: ${t.message}") }
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
        Log.i(TAG, "endSession done")
    }

    fun isRunning(): Boolean = _state.value.running

    override fun onCleared() {
        endSession()
    }
}
