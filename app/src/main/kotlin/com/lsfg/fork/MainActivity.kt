package com.lsfg.fork

import android.content.Context
import android.content.Intent
import android.os.Build
import android.os.Bundle
import android.provider.Settings
import android.util.Log
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
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
        Log.i(TAG, "MainActivity onCreate, session running=" + session.isRunning())

        setContent {
            LSFGTheme {
                Surface(
                    modifier = Modifier.fillMaxSize(),
                    color = MaterialTheme.colorScheme.background
                ) {
                    HomeScreen(
                        onStart = { startSession() },
                        onStop = { stopSession() }
                    )
                }
            }
        }
    }

    private fun startSession() {
        val s = session.state.value
        val pkg = s.targetPkg

        if (session.isRunning()) {
            Toast.makeText(this, "Session already running", Toast.LENGTH_SHORT).show()
            return
        }

        val overlayOk = if (Build.VERSION.SDK_INT >= 23)
            Settings.canDrawOverlays(this)
        else true
        if (!overlayOk) {
            Toast.makeText(this, "Grant overlay permission first (system UI settings)", Toast.LENGTH_LONG).show()
            try {
                startActivity(
                    Intent(android.provider.Settings.ACTION_MANAGE_OVERLAY_PERMISSION)
                        .apply { addFlags(Intent.FLAG_ACTIVITY_NEW_TASK) }
                )
            } catch (_: Throwable) {}
            return
        }

        if (pkg != null) {
            val launched = ShellBridge.launch(pkg)
            Log.i(TAG, "launch $pkg -> $launched")
        }

        // Delay so the target app reaches the foreground, then open the overlay.
        window.decorView.postDelayed({
            try {
                startActivity(
                    Intent(this, OverlayActivity::class.java)
                        .apply { addFlags(Intent.FLAG_ACTIVITY_NEW_TASK) }
                )
            } catch (t: Throwable) {
                Log.e(TAG, "start overlay failed", t)
                Toast.makeText(this, "Overlay failed: " + t.message, Toast.LENGTH_LONG).show()
            }
        }, 350)
    }

    private fun stopSession() {
        session.endSession()
        Toast.makeText(this, "Session stopped", Toast.LENGTH_SHORT).show()
    }

    companion object {
        const val TAG = "lsfg-fork"
    }
}
