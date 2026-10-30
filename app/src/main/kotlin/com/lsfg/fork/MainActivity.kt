package com.lsfg.fork

import android.Manifest
import android.app.Activity
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.provider.Settings
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import com.lsfg.fork.ui.AboutScreen
import com.lsfg.fork.ui.ConfigScreen
import com.lsfg.fork.ui.HomeScreen
import com.lsfg.fork.ui.LogsScreen
import com.lsfg.fork.overlay.OverlayHost
import com.lsfg.fork.ui.LSFGTheme
import com.lsfg.fork.ui.TestScreen

class MainActivity : ComponentActivity() {

    private val session: SessionManager get() = SessionManager.shared

    /** Re-enter the start flow after an async permission grant. */
    private var pendingStart = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        Log.i(SessionManager.TAG, "MainActivity onCreate, running=${session.isRunning()}")
        OverlayHost.bind(applicationContext)

        setContent {
            LSFGTheme {
                Surface(
                    modifier = Modifier.fillMaxSize(),
                    color = MaterialTheme.colorScheme.background
                ) {
                    TabRoot()
                }
            }
        }
    }

    // ==================================================================
    //  Tabbed root: Home | Test | Config | Logs | About
    // ==================================================================

    @Composable
    private fun TabRoot() {
        var tab by rememberSaveable { mutableIntStateOf(0) }
        val tabs = listOf("Home", "Test", "Config", "Logs", "About")
        Column(Modifier.fillMaxSize()) {
            Row(
                Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 8.dp, vertical = 6.dp)
            ) {
                tabs.forEachIndexed { i, label ->
                    Text(
                        text = label,
                        modifier = Modifier
                            .weight(1f)
                            .padding(vertical = 6.dp)
                            .clickable { tab = i },
                        style = MaterialTheme.typography.labelLarge,
                        color = if (tab == i) MaterialTheme.colorScheme.primary
                               else MaterialTheme.colorScheme.onSurfaceVariant,
                        textAlign = TextAlign.Center
                    )
                }
            }
            when (tab) {
                0 -> HomeScreen(
                    onStart = { startFlow() },
                    onStop = { stopFlow() },
                    onPickDll = { pickDll() },
                    onResetDll = { resetDll() }
                )
                1 -> TestScreen()
                2 -> ConfigScreen(onStart = { startFlow() })
                3 -> LogsScreen()
                else -> AboutScreen()
            }
        }
    }

    // ==================================================================
    //  Start / stop flows (fixed: permissions FIRST, then attach, then run)
    // ==================================================================

    private fun startFlow() {
        Log.i(SessionManager.TAG, "startFlow requested")
        if (session.isRunning()) {
            toast("Session already running")
            return
        }
        val s = session.state.value

        // (1) Notifications (Android 13+). Needed for the sticky control bar
        //     (Stop button) so the user can always stop the session (bug 1).
        if (Build.VERSION.SDK_INT >= 33) {
            val granted = checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS)
            if (granted != PackageManager.PERMISSION_GRANTED) {
                Log.i(SessionManager.TAG, "requesting POST_NOTIFICATIONS")
                pendingStart = true
                notificationPermissionLauncher.launch(Manifest.permission.POST_NOTIFICATIONS)
                return
            }
        }

        // (2) Overlay permission. Must be granted BEFORE the session so the
        //     presenter has a surface to draw into (bug 2 / bug 7).
        if (Build.VERSION.SDK_INT >= 23 && !Settings.canDrawOverlays(this)) {
            Log.i(SessionManager.TAG, "overlay permission missing -> opening settings")
            toast("Grant 'Display over other apps' to continue")
            try {
                startActivity(
                    Intent(Settings.ACTION_MANAGE_OVERLAY_PERMISSION)
                        .setPackage(packageName)
                        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                )
            } catch (_: Throwable) {
                try {
                    startActivity(
                        Intent(Settings.ACTION_MANAGE_OVERLAY_PERMISSION)
                            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                    )
                } catch (_: Throwable) {}
            }
            return
        }

        // (3) Nonroot needs MediaProjection consent (one-time system dialog).
        //     Must be obtained BEFORE the session starts (bug 2).
        if (s.captureMode == "nonroot" && SessionManager.pendingProjectionData == null) {
            Log.i(SessionManager.TAG, "requesting MediaProjection consent")
            toast("Allow screen capture (one-time consent), then we'll continue")
            // CRITICAL FIX (Jan: "picker -> app stops working without capture
            // being initiated"): the consent picker is a separate system
            // activity that puts LSFG in the background. On Android 14/15/17 the
            // system may kill a background app that has no active mediaProjection
            // foreground service. So BEFORE the picker opens, start the service
            // with the mediaProjection FGS type to keep LSFG alive while the
            // user is in the picker and when they return.
            try {
                val i = Intent(this, CaptureService::class.java).setAction(CaptureService.ACTION_PREPARE)
                if (Build.VERSION.SDK_INT >= 26) startForegroundService(i) else startService(i)
                Log.i(SessionManager.TAG, "prepared FGS (mediaProjection) before consent picker")
            } catch (t: Throwable) {
                Log.w(SessionManager.TAG, "prepare FGS failed: ${t.message}")
            }
            pendingStart = true
            requestProjectionConsent()
            return
        }

        // (4) All permissions in place. Attach the overlay while the app is in
        //     the FOREGROUND (most reliable on MIUI), launch the target, then
        //     hand off to the foreground service (keep-alive + controls).
        proceedToStart(s)
    }

    private fun proceedToStart(s: SessionManager.State) {
        // Attach the overlay WINDOW now (fast, non-blocking, main thread).
        // The actual Surface becomes ready on the next main-thread frame and is
        // awaited on a background thread inside SessionManager.beginSession.
        val attached = OverlayHost.attach()
        if (!attached) {
            Log.e(SessionManager.TAG, "proceedToStart: overlay attach failed (window addView threw)")
            toast("Overlay attach failed - grant 'display over other apps' and retry")
            return
        }
        Log.i(SessionManager.TAG, "proceedToStart: overlay window attached ${OverlayHost.size().first}x${OverlayHost.size().second}")

        // Launch the target app (if chosen) so it becomes foreground. The
        // overlay (already attached) sits above it; the game keeps its touch.
        val pkg = s.targetPkg
        if (pkg != null) {
            try {
                val li = packageManager.getLaunchIntentForPackage(pkg)
                if (li != null) startActivity(li)
                else ShellBridge.launch(pkg)
            } catch (_: Throwable) {
                ShellBridge.launch(pkg)
            }
            Log.i(SessionManager.TAG, "proceedToStart: launched target $pkg")
        }

        // Hand off to the foreground service (keep-alive + notification + controls).
        // The service calls beginSession, which awaits the surface on a bg thread
        // before starting the Vulkan presenter.
        val dllPath = DllStore.userDllPath(applicationContext)
        startServiceIntent(CaptureService.ACTION_START, dllPath)
        toast("LSFG starting...")
    }

    private fun stopFlow() {
        Log.i(SessionManager.TAG, "stopFlow requested")
        startServiceIntent(CaptureService.ACTION_STOP, null)
        toast("Stopping session...")
    }

    private fun startServiceIntent(action: String, dllPath: String?) {
        try {
            val i = Intent(this, CaptureService::class.java).setAction(action)
            if (dllPath != null) i.putExtra(CaptureService.EXTRA_DLL, dllPath)
            if (Build.VERSION.SDK_INT >= 26) startForegroundService(i) else startService(i)
            Log.i(SessionManager.TAG, "started service action=$action dll=$dllPath")
        } catch (t: Throwable) {
            Log.e(SessionManager.TAG, "startService failed: ${t.message}")
            toast("Service start failed: ${t.message}")
        }
    }

    // ==================================================================
    //  MediaProjection consent
    // ==================================================================

    private fun requestProjectionConsent() {
        try {
            val mpm = getSystemService(Context.MEDIA_PROJECTION_SERVICE) as android.media.projection.MediaProjectionManager
            val intent = mpm.createScreenCaptureIntent()
            projectionLauncher.launch(intent)
        } catch (t: Throwable) {
            Log.e(SessionManager.TAG, "projection consent: ${t.message}")
            toast("MediaProjection unavailable: ${t.message}")
        }
    }

    // ==================================================================
    //  user DLL picker
    // ==================================================================

    private fun pickDll() {
        try {
            val i = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
                addCategory(Intent.CATEGORY_OPENABLE)
                type = "*/*"
                putExtra(Intent.EXTRA_MIME_TYPES, arrayOf("*/*"))
            }
            dllLauncher.launch(i)
        } catch (t: Throwable) {
            toast("File picker failed: ${t.message}")
        }
    }

    private fun resetDll() {
        DllStore.clear(applicationContext)
        toast("Using bundled lsfg-vk.dll")
    }

    private fun copyUserDll(uri: Uri) {
        try {
            val target = DllStore.userDllFile(applicationContext)
            contentResolver.openInputStream(uri).use { input ->
                if (input == null) { toast("Cannot read file"); return }
                target.outputStream().use { output -> input.copyTo(output) }
            }
            DllStore.setActive(applicationContext)
            Log.i(SessionManager.TAG, "user DLL loaded: ${target.name} (${target.length()}B)")
            toast("Shader DLL loaded: ${target.name} (${target.length()} bytes)")
        } catch (t: Throwable) {
            Log.e(SessionManager.TAG, "copyUserDll: ${t.message}")
            toast("DLL copy failed: ${t.message}")
        }
    }

    // ==================================================================
    //  result launchers
    // ==================================================================

    private val notificationPermissionLauncher =
        registerForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
            Log.i(SessionManager.TAG, "POST_NOTIFICATIONS granted=$granted")
            if (pendingStart) {
                pendingStart = false
                // Re-run the start flow; it will now pass the notification check
                // and continue (overlay, consent, attach, start).
                startFlow()
            }
        }

    private val projectionLauncher =
        registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
            if (result.resultCode == Activity.RESULT_OK) {
                val data = result.data
                if (data != null) {
                    SessionManager.pendingProjectionData = data
                    Log.i(SessionManager.TAG, "MediaProjection consent granted")
                    // ALWAYS resume the start flow on consent (not only when
                    // pendingStart is set): the activity may have been recreated
                    // while the picker was open, which would have lost the flag.
                    // If a session is already running this is a harmless no-op.
                    pendingStart = false
                    if (!session.isRunning()) {
                        toast("Screen capture consented. Starting...")
                        startFlow()
                    }
                }
            } else {
                pendingStart = false
                Log.w(SessionManager.TAG, "MediaProjection consent DENIED")
                toast("Screen capture denied - use Root mode or re-try")
            }
        }

    private val dllLauncher =
        registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
            if (result.resultCode == Activity.RESULT_OK) {
                val uri = result.data?.data
                if (uri != null) copyUserDll(uri)
            }
        }

    private fun toast(msg: String) {
        Toast.makeText(this, msg, Toast.LENGTH_SHORT).show()
    }
}
