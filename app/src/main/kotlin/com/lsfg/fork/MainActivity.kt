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
import android.util.Log
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.ui.Modifier
import com.lsfg.fork.ui.HomeScreen
import com.lsfg.fork.ui.LSFGTheme

class MainActivity : ComponentActivity() {

    private val session: SessionManager get() = SessionManager.shared

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        Log.i(SessionManager.TAG, "MainActivity onCreate, running=" + session.isRunning())

        setContent {
            LSFGTheme {
                Surface(
                    modifier = Modifier.fillMaxSize(),
                    color = MaterialTheme.colorScheme.background
                ) {
                    HomeScreen(
                        onStart = { startFlow() },
                        onStop = { stopFlow() },
                        onPickDll = { pickDll() },
                        onResetDll = { resetDll() }
                    )
                }
            }
        }
    }

    // ---------- start / stop ----------

    private fun startFlow() {
        val s = session.state.value

        if (session.isRunning()) {
            toast("Session already running")
            return
        }

        // 1) Notification permission (Android 13+) - required for the sticky control UI.
        if (Build.VERSION.SDK_INT >= 33) {
            val granted = checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS)
            if (granted != PackageManager.PERMISSION_GRANTED) {
                notifiPermissionLauncher.launch(Manifest.permission.POST_NOTIFICATIONS)
                // continue anyway; the app still works, the notification may be blocked by the user
            }
        }

        // 2) Overlay permission (draw over other apps) - required for the visual overlay.
        if (Build.VERSION.SDK_INT >= 23 && !Settings.canDrawOverlays(this)) {
            toast("Grant 'display over other apps' to continue")
            try {
                startActivity(
                    Intent(Settings.ACTION_MANAGE_OVERLAY_PERMISSION)
                        .apply { addFlags(Intent.FLAG_ACTIVITY_NEW_TASK) }
                )
            } catch (_: Throwable) {}
            return
        }

        // 3) Nonroot mode needs MediaProjection consent (system dialog, per session).
        if (s.captureMode == "nonroot" && SessionManager.pendingProjectionData == null) {
            toast("Please allow screen capture (one-time consent)")
            requestProjectionConsent()
            return
        }

        // 4) Launch the target app (if selected), then start the service session.
        val pkg = s.targetPkg
        if (pkg != null) {
            try {
                val li = packageManager.getLaunchIntentForPackage(pkg)
                if (li != null) startActivity(li) else ShellBridge.launch(pkg)
            } catch (_: Throwable) {
                ShellBridge.launch(pkg)
            }
        }

        val dllPath = DllStore.userDllPath(applicationContext)
        window.decorView.postDelayed({
            startServiceIntent(CaptureService.ACTION_START, dllPath)
            toast("LSFG starting...")
        }, if (pkg != null) 500L else 50L)
    }

    private fun stopFlow() {
        startServiceIntent(CaptureService.ACTION_STOP, null)
        toast("Stopping session...")
    }

    private fun startServiceIntent(action: String, dllPath: String?) {
        try {
            val i = Intent(this, CaptureService::class.java).setAction(action)
            if (dllPath != null) i.putExtra(CaptureService.EXTRA_DLL, dllPath)
            if (Build.VERSION.SDK_INT >= 26) startForegroundService(i) else startService(i)
        } catch (t: Throwable) {
            Log.e(SessionManager.TAG, "startService", t)
            toast("Service start failed: ${t.message}")
        }
    }

    // ---------- MediaProjection consent ----------

    private fun requestProjectionConsent() {
        try {
            val mpm = getSystemService(Context.MEDIA_PROJECTION_SERVICE) as android.media.projection.MediaProjectionManager
            val intent = mpm.createScreenCaptureIntent()
            projectionLauncher.launch(intent)
        } catch (t: Throwable) {
            Log.e(SessionManager.TAG, "projection consent", t)
            toast("MediaProjection unavailable: " + t.message)
        }
    }

    // ---------- user DLL picker ----------

    private fun pickDll() {
        try {
            val i = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
                addCategory(Intent.CATEGORY_OPENABLE)
                type = "*/*"
                putExtra(Intent.EXTRA_MIME_TYPES, arrayOf("*/*"))
            }
            dllLauncher.launch(i)
        } catch (t: Throwable) {
            toast("File picker failed: " + t.message)
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
            toast("Shader DLL loaded: ${target.name} (${target.length()} bytes)")
        } catch (t: Throwable) {
            Log.e(SessionManager.TAG, "copyUserDll", t)
            toast("DLL copy failed: " + t.message)
        }
    }

    // ---------- result launchers ----------

    private val notifiPermissionLauncher =
        registerForActivityResult(ActivityResultContracts.RequestPermission()) { }

    private val projectionLauncher =
        registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
            if (result.resultCode == Activity.RESULT_OK) {
                val data = result.data
                if (data != null) {
                    SessionManager.pendingProjectionData = data
                    toast("Screen capture consented. Starting...")
                    window.decorView.postDelayed({
                        startServiceIntent(CaptureService.ACTION_START, DllStore.userDllPath(applicationContext))
                    }, 200L)
                }
            } else {
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
