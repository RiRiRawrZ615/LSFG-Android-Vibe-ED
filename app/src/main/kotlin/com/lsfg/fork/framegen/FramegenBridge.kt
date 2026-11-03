package com.lsfg.fork.framegen

/** JNI bridge for the LSFG v3_1 compute pipeline (framegen). */
object FramegenBridge {

    private external fun nativeSetCacheDir(dir: String)
    private external fun nativeInit(dllPath: String, allowFP16: Boolean): Long
    private external fun nativeCreateSession(
        handle: Long,
        width: Int, height: Int, flowScale: Float, performance: Boolean
    ): Boolean

    private external fun nativeWriteSource(handle: Long, srcAhb: Long, slot: Int)
    private external fun nativeDispatch(handle: Long, total: Int)
    private external fun nativeAcquire(handle: Long)
    private external fun nativeAcquireToDst(handle: Long, dstAhb: Long)
    private external fun nativeIdle(handle: Long)
    private external fun nativeLastError(handle: Long): String
    private external fun nativeDestinationFd(handle: Long): IntArray
    private external fun nativeRingCount(handle: Long): Int
    private external fun nativeRingAhb(handle: Long, index: Int): Long
    private external fun nativeDestroy(handle: Long)

    init {
        try {
            System.loadLibrary("lsfg_framegen")
        } catch (t: Throwable) {
            println("FramegenBridge: failed to load lsfg_framegen: $t")
        }
    }

    var loaded: Boolean = false
        private set

    /**
     * Set the pipeline-cache directory BEFORE init. Must be a directory the
     * process can always write to (app internal files dir on Android).
     * Never /tmp/ (read-only tmpfs on HyperOS even with root).
     */
    fun setCacheDir(dir: String) {
        try { nativeSetCacheDir(dir) } catch (t: Throwable) { println("FramegenBridge.setCacheDir: $t") }
    }

    @Synchronized
    fun init(dllPath: String, allowFP16: Boolean): Boolean {
        val h = try {
            nativeInit(dllPath, allowFP16)
        } catch (t: Throwable) {
            println("FramegenBridge.init: $t")
            0L
        }
        loaded = h != 0L
        instance = h
        return loaded
    }

    private var instance = 0L

    fun createSession(
        width: Int, height: Int,
        flowScale: Float, performance: Boolean
    ): Boolean {
        if (instance == 0L) return false
        return try {
            nativeCreateSession(instance, width, height, flowScale, performance)
        } catch (t: Throwable) {
            println("FramegenBridge.createSession: $t")
            false
        }
    }

    fun writeSource(srcAhb: Long, slot: Int) {
        if (instance == 0L) return
        try { nativeWriteSource(instance, srcAhb, slot) } catch (_: Throwable) {}
    }

    fun dispatch(total: Int) {
        if (instance == 0L) return
        try { nativeDispatch(instance, total) } catch (_: Throwable) {}
    }

    fun acquire() {
        if (instance == 0L) return
        try { nativeAcquire(instance) } catch (_: Throwable) {}
    }

    /** Acquire the next generated frame and blit it into the shared dst AHB. */
    fun acquireToDst(dstAhb: Long) {
        if (instance == 0L) return
        try { nativeAcquireToDst(instance, dstAhb) } catch (_: Throwable) {}
    }

    fun idle() {
        if (instance == 0L) return
        try { nativeIdle(instance) } catch (_: Throwable) {}
    }

    /**
     * Openable fd for the pipeline's destination image, or null.
     *
     * This is the fd-based frame exchange: the presenter imports this fd instead
     * of the framegen copying each generated frame into an app-allocated
     * AHardwareBuffer. Returns (fd, width, height); the caller owns the fd and
     * must close it.
     */
    fun destinationFd(): Triple<Int, Int, Int>? {
        if (instance == 0L) return null
        return try {
            val a = nativeDestinationFd(instance)
            if (a == null || a.size < 3 || a[0] < 0) null
            else Triple(a[0], a[1], a[2])
        } catch (_: Throwable) { null }
    }

    /**
     * Frame-exchange ring AHardwareBuffer at [index], or 0 when unavailable.
     *
     * These are destination images created by the framegen device itself and
     * exported as AHBs. The presenter imports them and copies them into the
     * swapchain, so the GPU never writes into an app-allocated buffer (the
     * operation that faults inside Adreno's copy helper on this GPU).
     *
     * The returned pointer must NOT be released: the framegen session owns the
     * export reference and releases it on destroy.
     */
    fun ringAhb(index: Int): Long {
        if (instance == 0L) return 0L
        return try { nativeRingAhb(instance, index) } catch (_: Throwable) { 0L }
    }

    /** Number of ring slots, or 0 when the ring is unavailable. */
    fun ringCount(): Int {
        if (instance == 0L) return 0
        return try { nativeRingCount(instance) } catch (_: Throwable) { 0 }
    }

    /**
     * Last error reported by the native frame path, or null when healthy.
     * The frame-path JNI calls cannot throw back into Kotlin (they catch
     * std::exception and log), so this is how a stalled or failed GPU path
     * becomes visible to the UI instead of a silent 0 fps.
     */
    fun lastError(): String? {
        if (instance == 0L) return null
        return try {
            nativeLastError(instance).takeIf { it.isNotBlank() }
        } catch (_: Throwable) { null }
    }

    @Synchronized
    fun destroy() {
        if (instance == 0L) return
        try { nativeDestroy(instance) } catch (_: Throwable) {}
        instance = 0L
        loaded = false
    }
}
