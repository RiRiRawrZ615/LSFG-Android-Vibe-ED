package com.lsfg.fork

import android.app.Activity
import android.graphics.PixelFormat
import android.os.Build
import android.os.Bundle
import android.util.DisplayMetrics
import android.view.Gravity
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.WindowManager
import android.widget.FrameLayout
import android.widget.TextView
import com.lsfg.fork.wsi.WsiBridge

/**
 * Transparent overlay activity that hosts the WSI presenter surface.
 *
 * Layout: the lower half of the screen hosts a [SurfaceView] (the
 * presentation target for captured/generated frames). The upper half is
 * left fully transparent so the target game underneath stays visible.
 * A small HUD line shows FPS + capture latency.
 */
class OverlayActivity : Activity() {

    private lateinit var holder: SurfaceHolder
    private lateinit var surfaceView: SurfaceView
    private lateinit var hud: TextView
    private var started = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        requestWindowFeature(WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN)
        val a = window.attributes.apply {
            type = if (Build.VERSION.SDK_INT >= 23)
                WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY
            else
                WindowManager.LayoutParams.TYPE_PHONE
            format = android.graphics.PixelFormat.TRANSLUCENT
            flags = flags or
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE or
                WindowManager.LayoutParams.FLAG_HARDWARE_ACCELERATED
            gravity = Gravity.TOP or Gravity.START
            width = WindowManager.LayoutParams.MATCH_PARENT
            height = WindowManager.LayoutParams.MATCH_PARENT
        }
        window.attributes = a
        window.addFlags(WindowManager.LayoutParams.FLAG_HARDWARE_ACCELERATED)

        val root = FrameLayout(this)
        val dm = DisplayMetrics()
        @Suppress("DEPRECATION")
        windowManager.defaultDisplay.getMetrics(dm)
        val halfH = dm.heightPixels / 2

        surfaceView = SurfaceView(this)
        surfaceView.holder.setFormat(PixelFormat.RGBA_8888)
        val slp = FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT, halfH, Gravity.BOTTOM
        )
        root.addView(surfaceView, slp)

        hud = TextView(this)
        hud.text = "LSFG overlay: waiting for surface"
        hud.setTextColor(0xFF22D3EE.toInt())
        hud.textSize = 13f
        val hlp = FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.WRAP_CONTENT,
            FrameLayout.LayoutParams.WRAP_CONTENT,
            Gravity.TOP or Gravity.START
        )
        hlp.setMargins(16, 60, 0, 0)
        root.addView(hud, hlp)

        setContentView(root)

        holder = surfaceView.holder
        holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(s: SurfaceHolder) {
                onSurfaceReady(s)
            }
            override fun surfaceChanged(s: SurfaceHolder, format: Int, w: Int, h: Int) {
                // size is fixed for this session
            }
            override fun surfaceDestroyed(s: SurfaceHolder) {
                if (started) {
                    WsiBridge.stop()
                    started = false
                }
            }
        })
    }

    private fun onSurfaceReady(s: SurfaceHolder) {
        if (started) return
        started = true
        val sm = SessionManager.shared
        val s2 = s.surface ?: return
        val windowPtr = WsiBridge.nativeSurfaceToWindow(s2)
        if (windowPtr == 0L) {
            hud.text = "LSFG: surface->window failed"
            return
        }
        surfaceView.post {
            val w = surfaceView.width
            val h = surfaceView.height
            sm.beginSession(windowPtr, s2, w, h, this)
            hud.text = "LSFG: presenting ${w}x$h (passthrough)"
            val job = object : Runnable {
                var last = 0L
                override fun run() {
                    if (!isFinishing) {
                        val st = sm.state.value
                        if (st.running) {
                            val now = System.currentTimeMillis()
                            if (now - last > 500) {
                                last = now
                                hud.text = "LSFG: %.1f fps, cap %.1f ms, %s".format(
                                    st.fps, st.captureLatencyMs, st.statusMsg
                                )
                            }
                        }
                        surfaceView.postDelayed(this, 500)
                    }
                }
            }
            surfaceView.postDelayed(job, 500)
        }
    }

    companion object {
        const val EXTRA_WIDTH = "extra_width"
        const val EXTRA_HEIGHT = "extra_height"
    }
}
