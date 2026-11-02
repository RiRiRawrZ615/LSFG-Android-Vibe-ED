package com.lsfg.fork.session

import android.hardware.HardwareBuffer
import com.lsfg.fork.wsi.WsiBridge

/**
 * JNI entry points used by the ported LSFG-Android session/ capture skeleton
 * (RootCaptureEngine / ShizukuCaptureEngine). In the reference app this object
 * is the single native bridge into liblsfg-android.so; in this fork the native
 * side is split between liblsfg_wsi.so (WsiBridge) and liblsfg_framegen.so
 * (FramegenBridge), so this object routes the calls accordingly.
 */
object NativeBridge {

    /**
     * Session-level frame sink set by SessionManager: receives every captured
     * frame (framegen drive path). When null (no session / test pattern),
     * frames go straight to the WSI presenter (passthrough).
     */
    @Volatile
    var sessionFrameSink: ((HardwareBuffer) -> Unit)? = null

    /**
     * Hands a fresh capture frame (HardwareBuffer) to the pipeline. The
     * sink/presenter takes its own reference, so the caller may close the AHB
     * right after this returns.
     */
    fun pushFrame(hardwareBuffer: HardwareBuffer, timestampNs: Long) {
        val sink = sessionFrameSink
        if (sink != null) {
            try {
                sink(hardwareBuffer)
                return
            } catch (t: Throwable) {
                LsfgLog.w(TAG, "sessionFrameSink threw: ${t.message}")
            }
        }
        val ptr = WsiBridge.nativeAhbPtr(hardwareBuffer)
        if (ptr == 0L) {
            LsfgLog.w(TAG, "pushFrame: AHB ptr == 0 (${hardwareBuffer.width}x${hardwareBuffer.height})")
            return
        }
        WsiBridge.nativePushFrame(ptr)
    }

    /**
     * Frames produced by framegen since the last session. The fork's
     * FramegenBridge has no dedicated counter; framegen frames are posted
     * through the same WSI path, so the posted count already includes them.
     * 0 here only disables the frame-graph's generated-fps axis.
     */
    fun getGeneratedFrameCount(): Long = 0L

    /** Total frames actually posted to the overlay surface (ground truth). */
    fun getPostedFrameCount(): Long =
        try { WsiBridge.framesPresented() } catch (t: Throwable) { 0L }

    /**
     * Capture frames whose pixel content differs from the previous capture
     * (the target app's true render rate). The fork's WSI has no native
     * dedupe counter yet, so this reports posted frames; the FPS UI is
     * unaffected in practice because the capture loop paces the source.
     */
    fun getUniqueCaptureCount(): Long = getPostedFrameCount()

    /** Shizuku timing side channel (pacing only, never the visible source). */
    fun reportShizukuTiming(timestampNs: Long, frameTimeNs: Long, pacingJitterNs: Long) {
        // Accepted but unused in the fork: pacing is driven by the capture
        // loop's own interval, not by Shizuku timing samples.
    }

    private const val TAG = "NativeBridge"
}
