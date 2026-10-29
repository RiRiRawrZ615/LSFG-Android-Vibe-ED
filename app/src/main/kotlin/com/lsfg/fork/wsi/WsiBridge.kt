package com.lsfg.fork.wsi

import android.hardware.HardwareBuffer
import android.view.Surface

/** JNI bridge for the Vulkan WSI presenter (overlay swapchain). */
object WsiBridge {

    init {
        System.loadLibrary("lsfg_wsi")
    }

    private external fun nativeStart(windowPtr: Long, width: Int, height: Int, targetFps: Float)
    private external fun nativeStop()
    private external fun nativeIsRunning(): Boolean
    private external fun nativeSetPaused(paused: Boolean)
    private external fun nativeSetTargetFps(fps: Float)

    /** Convert a Java HardwareBuffer to a native AHardwareBuffer* (no ref). */
    external fun nativeAhbPtr(buffer: HardwareBuffer): Long

    /** Push a native AHardwareBuffer* into the frame queue. */
    external fun nativePushFrame(ahbPtr: Long)

    external fun nativeSurfaceToWindow(surface: Surface): Long
    external fun nativeReleaseWindow(windowPtr: Long)

    fun start(windowPtr: Long, width: Int, height: Int, targetFps: Float) =
        nativeStart(windowPtr, width, height, targetFps)

    fun stop() = nativeStop()
    fun isRunning() = nativeIsRunning()
    fun setPaused(paused: Boolean) = nativeSetPaused(paused)
    fun setTargetFps(fps: Float) = nativeSetTargetFps(fps)
}
