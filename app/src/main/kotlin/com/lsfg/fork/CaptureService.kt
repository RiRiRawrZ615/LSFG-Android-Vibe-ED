package com.lsfg.fork

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.util.Log
import androidx.core.app.NotificationCompat
import com.lsfg.fork.overlay.OverlayHost
import kotlin.concurrent.thread

/**
 * Foreground service that keeps the LSFG session alive:
 *  - keeps the process alive on Xiaomi/MIUI (sticky foreground notification);
 *  - owns the visual-only overlay via the shared [OverlayHost];
 *  - exposes sticky notification controls (Start/Stop, multiplier, mode, open
 *    app) so the user can always stop the session without touching the game
 *    (bug 1).
 *
 * The overlay is attached from MainActivity while the app is foreground
 * (most reliable on MIUI). This service only keeps the session running and
 * provides controls, so it never re-attaches a second overlay window.
 */
class CaptureService : Service() {

    companion object {
        const val TAG = "lsfg-service"
        const val CHANNEL = "lsfg_session"
        const val NOTIF = 4242

        const val ACTION_START = "com.lsfg.fork.START"
        const val ACTION_STOP = "com.lsfg.fork.STOP"
        const val ACTION_TOGGLE = "com.lsfg.fork.TOGGLE"
        const val ACTION_MULT = "com.lsfg.fork.MULT"
        const val ACTION_MODE = "com.lsfg.fork.MODE"
        const val ACTION_OPEN = "com.lsfg.fork.OPEN"

        const val EXTRA_DLL = "user_dll_path"
    }

    private lateinit var nm: NotificationManager
    private val main = Handler(Looper.getMainLooper())
    private val session: SessionManager get() = SessionManager.shared

    private var userDllPath: String? = null
    private var tickJob: Thread? = null
    @Volatile private var usingMediaProjection = false

    private data class Multi(val value: Int, val label: String)
    private val multis = listOf(Multi(1, "1x"), Multi(2, "2x"), Multi(3, "3x"), Multi(4, "4x"))
    private val modes = listOf("root", "nonroot", "shizuku")

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        nm = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        createChannel()
        OverlayHost.bind(applicationContext)
        Log.i(TAG, "service onCreate")
        startForegroundSafely(buildIdleNotification("Ready."))
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        Log.i(TAG, "onStartCommand action=${intent?.action} dll=${intent?.getStringExtra(EXTRA_DLL)}")
        userDllPath = intent?.getStringExtra(EXTRA_DLL)

        when (intent?.action) {
            ACTION_START, null -> startSession()
            ACTION_STOP -> stopSession()
            ACTION_TOGGLE -> {
                if (session.isRunning()) stopSession() else startSession()
            }
            ACTION_MULT -> cycleMultiplier()
            ACTION_MODE -> cycleMode()
            ACTION_OPEN -> openApp()
            else -> startForegroundSafely(buildIdleNotification())
        }
        return START_STICKY
    }

    // ---------- session control ----------

    private fun startSession() {
        val sm = session
        if (sm.isRunning()) {
            Log.i(TAG, "startSession: already running")
            updateNotificationStatus("already running")
            return
        }
        updateNotificationStatus("starting (overlay + Vulkan + capture)...")
        // All heavy setup (surface await, Vulkan start, capture engine) runs on a
        // background thread so it can BLOCK waiting for the overlay surface
        // without deadlocking the main thread's surfaceCreated callback.
        //
        // CRITICAL FIX (crash: java.lang.IllegalThreadStateException at
        // CaptureService.startSession -> Thread.start): the old code used
        // kotlin.concurrent.thread { ... } which ALREADY starts the thread
        // (default start=true), and then called .start() on the returned Thread
        // a SECOND time -> "can't start a thread that has already been started"
        // -> the service was killed on the very first real start. Use a plain
        // Thread with a single .start() call.
        val beginThread = Thread({ runBegin(sm) }, "lsfg-begin")
        beginThread.isDaemon = true
        beginThread.start()
    }

    /**
     * Runs on a background thread: attach the overlay if needed, await the
     * surface (blocking is allowed here, it is NOT the main thread), then
     * start the session.  Extracted into a function so `return` works cleanly
     * (a `return` inside a lambda passed to Thread() is "prohibited").
     */
    private fun runBegin(sm: SessionManager) {
        try {
            // Ensure the overlay window exists (fast, non-blocking). attach()
            // must run on the main thread (WindowManager.addView), so post it
            // there; then wait for the surface on THIS background thread.
            if (!OverlayHost.isAttached()) {
                main.post { OverlayHost.attach() }
                // give addView a moment to be enqueued
                Thread.sleep(50)
            }
            val surf = OverlayHost.awaitSurface(6000L)
            if (surf == null) {
                Log.e(TAG, "startSession: overlay surface never ready")
                updateNotificationStatus("overlay surface never ready - grant 'display over other apps'")
                return
            }
            val (w, h) = OverlayHost.size()
            Log.i(TAG, "startSession: surface ready ${w}x${h} mode=${sm.state.value.captureMode}")
            usingMediaProjection = sm.state.value.captureMode == "nonroot"
            sm.beginSession(surf, w, h, applicationContext, userDllPath)
            startStatusTicker()
            updateNotificationStatus("running: ${sm.state.value.captureMode} -> overlay ${w}x${h}")
        } catch (t: Throwable) {
            Log.e(TAG, "startSession crashed: ${t.message}")
            updateNotificationStatus("start failed: ${t.message}")
        }
    }

    private fun stopSession() {
        Log.i(TAG, "stopSession")
        main.post {
            try { session.endSession() } catch (t: Throwable) { Log.e(TAG, "endSession", t) }
            usingMediaProjection = false
            // Detach the overlay so it fully releases the window (the user is
            // done; the activity can re-attach on the next start).
            try { OverlayHost.detach() } catch (_: Throwable) {}
            stopStatusTicker()
            startForegroundSafely(buildIdleNotification("Stopped."))
        }
        updateNotificationStatus("stopped")
    }

    private fun cycleMultiplier() {
        val sm = session
        val cur = sm.state.value.multiplier
        val next = multis.firstOrNull { it.value > cur } ?: multis.first()
        sm.setMultiplier(next.value)
        updateNotificationStatus("multiplier -> ${next.label}")
    }

    private fun cycleMode() {
        val sm = session
        val cur = sm.state.value.captureMode
        val next = modes[(modes.indexOf(cur) + 1).let { if (it >= modes.size) 0 else it }]
        sm.setCaptureMode(next)
        if (sm.isRunning()) {
            main.post {
                val surf = OverlayHost.surface ?: run {
                    updateNotificationStatus("mode switch failed: overlay gone")
                    return@post
                }
                try {
                    sm.endSession()
                    val (w, h) = OverlayHost.size()
                    usingMediaProjection = next == "nonroot"
                    sm.beginSession(surf, w, h, applicationContext, userDllPath)
                    updateNotificationStatus("capture mode -> $next (restarted)")
                } catch (t: Throwable) {
                    updateNotificationStatus("mode switch failed: ${t.message}")
                }
            }
        } else {
            updateNotificationStatus("capture mode -> $next (applies on start)")
        }
    }

    private fun openApp() {
        try {
            val i = Intent(this, MainActivity::class.java).apply {
                addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            }
            startActivity(i)
        } catch (t: Throwable) {
            Log.e(TAG, "openApp", t)
        }
    }

    // ---------- status ticker (keeps the notification live) ----------

    private fun startStatusTicker() {
        if (tickJob != null) return
        tickJob = thread(name = "lsfg-notif-tick") {
            while (true) {
                try { Thread.sleep(1000) } catch (_: InterruptedException) { break }
                if (!session.isRunning()) break
                val s = session.state.value
                val text = if (s.fps > 0f)
                    "%.0f fps  |  %s  |  mult x%d  |  q:%d"
                        .format(s.fps, s.statusMsg, s.multiplier, s.queueDepth)
                else
                    "starting...  |  ${s.statusMsg}"
                main.post { updateNotificationStatus(text) }
            }
        }
    }

    private fun stopStatusTicker() {
        try { tickJob?.interrupt() } catch (_: Throwable) {}
        tickJob = null
    }

    // ---------- notifications ----------

    private fun createChannel() {
        val ch = NotificationChannel(CHANNEL, "LSFG session", NotificationManager.IMPORTANCE_LOW)
        ch.description = "LSFG frame generation session controls"
        ch.setShowBadge(false)
        nm.createNotificationChannel(ch)
    }

    private fun startForegroundSafely(n: Notification) {
        try {
            if (Build.VERSION.SDK_INT >= 33) {
                // API 33+: always declare a valid FGS type (specialUse base),
                // adding mediaProjection only while a MediaProjection is in use
                // (API 34+ enforces that declared mediaProjection is actually active).
                var type = ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE
                if (usingMediaProjection) {
                    type = type or ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION
                }
                startForeground(NOTIF, n, type)
            } else {
                startForeground(NOTIF, n)
            }
        } catch (e: Throwable) {
            Log.e(TAG, "startForeground", e)
        }
    }

    private fun buildIdleNotification(statusText: String = "Ready. Pick target app, then press Start."): Notification {
        val open = pendingAction(ACTION_OPEN, "Open LSFG", buildActionIntent(ACTION_OPEN))
        val start = pendingAction(ACTION_START, "Start", buildActionIntent(ACTION_START))
        return NotificationCompat.Builder(this, CHANNEL)
            .setSmallIcon(R.drawable.ic_fg_play)
            .setContentTitle("LSFG frame interpolation")
            .setContentText(statusText)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setContentIntent(open)
            .addAction(R.drawable.ic_fg_play, "Start", start)
            .addAction(R.drawable.ic_fg_open, "Open app", open)
            .build()
    }

    private fun buildRunningNotification(statusText: String): Notification {
        val s = session.state.value
        val open = pendingAction(ACTION_OPEN, "Open LSFG", buildActionIntent(ACTION_OPEN))
        val stop = pendingAction(ACTION_STOP, "Stop", buildActionIntent(ACTION_STOP))
        val mult = pendingAction(ACTION_MULT, "Mult ${multLabel()}", buildActionIntent(ACTION_MULT))
        val mode = pendingAction(ACTION_MODE, "Mode: ${s.captureMode}", buildActionIntent(ACTION_MODE))

        return NotificationCompat.Builder(this, CHANNEL)
            .setSmallIcon(R.drawable.ic_fg_play)
            .setContentTitle("LSFG running  ${multLabel()}  ${s.captureMode}")
            .setContentText(statusText)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setContentIntent(open)
            .addAction(R.drawable.ic_fg_stop, "Stop", stop)
            .addAction(R.drawable.ic_fg_mult, "Mult ${multLabel()}", mult)
            .addAction(R.drawable.ic_fg_mode, "Mode: ${s.captureMode}", mode)
            .build()
    }

    private fun multLabel(): String =
        multis.firstOrNull { it.value == session.state.value.multiplier }?.label ?: "2x"

    private fun updateNotificationStatus(text: String) {
        main.post {
            val n = if (session.isRunning()) buildRunningNotification(text)
            else buildIdleNotification(text)
            try {
                nm.notify(NOTIF, n)
                if (session.isRunning()) {
                    startForegroundSafely(n)
                }
            } catch (t: Throwable) {
                Log.e(TAG, "notify", t)
            }
        }
    }

    private fun pendingAction(action: String, label: String, intent: Intent): PendingIntent {
        var flags = PendingIntent.FLAG_UPDATE_CURRENT
        if (Build.VERSION.SDK_INT >= 23) flags = flags or PendingIntent.FLAG_IMMUTABLE
        return PendingIntent.getService(this, action.hashCode(), intent, flags)
    }

    private fun buildActionIntent(action: String): Intent =
        Intent(this, CaptureService::class.java).setAction(action)

    override fun onDestroy() {
        Log.i(TAG, "onDestroy")
        stopStatusTicker()
        try { session.endSession() } catch (_: Throwable) {}
        try { OverlayHost.detach() } catch (_: Throwable) {}
        super.onDestroy()
    }
}
