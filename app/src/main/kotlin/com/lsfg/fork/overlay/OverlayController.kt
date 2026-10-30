package com.lsfg.fork.overlay

import android.content.Context
import android.graphics.PixelFormat
import android.os.Build
import android.util.DisplayMetrics
import android.view.Gravity
import android.view.Surface
import android.view.SurfaceView
import android.view.WindowManager

/**
 * Hosts the presentation [SurfaceView] as a system overlay window
 * (TYPE_APPLICATION_OVERLAY) instead of a translucent activity.
 *
 * Why: a full-screen translucent *activity* covering a game is what MIUI/Xiaomi
 * starves or drops (the overlay "never draws" / crashes in the target app).
 * A real overlay window is the supported "draw over other apps" surface:
 *  - it never becomes the foreground activity, so the game keeps rendering;
 *  - it is visual-only (FLAG_NOT_TOUCHABLE) so it never steals taps;
 *  - it stays above the target app on MIUI.
 *
 * The [Surface] is handed to the WSI presenter, which acquires/releases its own
 * ANativeWindow reference around each session (balanced, no leaks/use-after-free).
 */
class OverlayController(private val appContext: Context) {

    private val wm: WindowManager =
        appContext.getSystemService(Context.WINDOW_SERVICE) as WindowManager

    private var surfaceView: SurfaceView? = null

    @Volatile
    var surface: Surface? = null
        private set

    private val dm = DisplayMetrics()

    fun displaySize(): Pair<Int, Int> {
        @Suppress("DEPRECATION")
        wm.defaultDisplay.getRealMetrics(dm)
        return dm.widthPixels to dm.heightPixels
    }

    /**
     * Attach the full-screen overlay surface and wait for it to be ready.
     * Returns true when [surface] is available (ready for the WSI presenter).
     * Must be called on the main thread (WindowManager + SurfaceView).
     */
    fun attach(): Boolean {
        if (surfaceView != null) return surface != null

        val (w, h) = displaySize()
        val sv = SurfaceView(appContext)
        sv.holder.setFormat(PixelFormat.RGBA_8888)

        val type = if (Build.VERSION.SDK_INT >= 23)
            WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY
        else
            @Suppress("DEPRECATION")
            WindowManager.LayoutParams.TYPE_PHONE

        val lp = WindowManager.LayoutParams(
            w, h,
            type,
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                    or WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE
                    or WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN
                    or WindowManager.LayoutParams.FLAG_HARDWARE_ACCELERATED,
            PixelFormat.TRANSLUCENT
        ).apply {
            gravity = Gravity.TOP or Gravity.START
        }

        try {
            wm.addView(sv, lp)
        } catch (t: Throwable) {
            android.util.Log.e("LSFG-Overlay", "addView failed", t)
            return false
        }
        surfaceView = sv

        sv.holder.addCallback(object : android.view.SurfaceHolder.Callback {
            override fun surfaceCreated(holder: android.view.SurfaceHolder) {
                surface = holder.surface
            }
            override fun surfaceChanged(holder: android.view.SurfaceHolder, format: Int, width: Int, height: Int) {}
            override fun surfaceDestroyed(holder: android.view.SurfaceHolder) {
                surface = null
            }
        })

        // Poll briefly for the surface (a SurfaceView in an overlay can take a frame).
        val deadline = System.currentTimeMillis() + 1500
        while (surface == null && System.currentTimeMillis() < deadline) {
            try { Thread.sleep(30) } catch (_: InterruptedException) { break }
        }
        return surface != null
    }

    fun detach() {
        try {
            surfaceView?.let { if (it.parent != null) wm.removeView(it) }
        } catch (t: Throwable) {
            android.util.Log.w("LSFG-Overlay", "removeView", t)
        }
        surface = null
        surfaceView = null
    }

    fun isAttached(): Boolean = surfaceView != null
}
