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
import android.provider.Settings
import android.util.Log
import androidx.core.app.NotificationCompat
import com.lsfg.fork.overlay.OverlayController
import kotlin.concurrent.thread

/**
 * Foreground service that owns the LSFG session:
 *  - keeps the process alive on Xiaomi/MIUI (sticky foreground notification);
 *  - hosts the visual-only overlay (TYPE_APPLICATION_OVERLAY) via [OverlayController];
 *  - exposes sticky notification controls: start/stop, multiplier cycle,
 *    capture-mode cycle, and "open app" to restore settings while gaming.
 *
 * Notification actions work without bringing the app to the foreground, so
 * the target game is never interrupted.
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
    private var overlay: OverlayController? = null
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
            updateNotificationStatus("already running")
            return
        }

        // Overlay permission check.
        if (Build.VERSION.SDK_INT >= 23 && !Settings.canDrawOverlays(this)) {
            updateNotificationStatus("overlay permission missing - open app")
            openApp()
            return
        }

        val ov = OverlayController(applicationContext)
        overlay = ov

        // attach() must run on the main thread (WindowManager + SurfaceView).
        main.post {
            try {
                val (w, h) = ov.displaySize()
                Log.i(TAG, "display ${w}x${h}, attaching overlay")
                val ok = ov.attach()
                val surf = ov.surface
                if (!ok || surf == null) {
                    updateNotificationStatus("overlay attach FAILED (grant overlay permission)")
                    openApp()
                    return@post
                }
                Log.i(TAG, "overlay attached, ${w}x${h}")
                usingMediaProjection = sm.state.value.captureMode == "nonroot"
                sm.beginSession(surf, w, h, applicationContext, userDllPath)
                startStatusTicker()
                updateNotificationStatus(
                    "running: ${sm.state.value.captureMode} -> overlay ${w}x${h}"
                )
            } catch (t: Throwable) {
                Log.e(TAG, "startSession crashed", t)
                updateNotificationStatus("error: ${t.message}")
            }
        }
    }

    private fun stopSession() {
        main.post {
            try {
                session.endSession()
            } catch (t: Throwable) {
                Log.e(TAG, "endSession", t)
            }
            usingMediaProjection = false
            try { overlay?.detach() } catch (_: Throwable) {}
            overlay = null
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
            // Apply live: full session restart (window ref is re-acquired per session).
            main.post {
                val ov = overlay ?: return@post
                val surf = ov.surface ?: run {
                    updateNotificationStatus("mode switch failed: overlay gone")
                    return@post
                }
                try {
                    sm.endSession()
                    val (w, h) = ov.displaySize()
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
                // API 29-32: type is taken from the manifest declaration.
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
                    // refresh FGS notification too (covers MIUI restarts)
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
        try { overlay?.detach() } catch (_: Throwable) {}
        super.onDestroy()
    }
}
