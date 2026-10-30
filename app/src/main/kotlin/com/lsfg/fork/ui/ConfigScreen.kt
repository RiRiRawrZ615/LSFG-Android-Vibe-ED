package com.lsfg.fork.ui

import android.content.Context
import android.content.Intent
import android.os.Build
import android.provider.Settings
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.lsfg.fork.Log
import com.lsfg.fork.RootChecker
import com.lsfg.fork.SessionManager
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/**
 * Configuration / permission tab (bug 2).
 *
 * Verifies that every permission the active capture mode needs is actually
 * granted BEFORE a session can start, and runs a real capture probe so the
 * user sees whether the chosen path will produce frames.
 */
@Composable
fun ConfigScreen(onStart: () -> Unit) {
    val ctx = LocalContext.current as Context
    val session = remember { SessionManager.shared }
    val s by session.state.collectAsState()
    val accent = Color(0xFF22D3EE)

    // Live permission / probe state.
    var overlayGranted by remember { mutableStateOf(android.provider.Settings.canDrawOverlays(ctx)) }
    var notifGranted by remember {
        mutableStateOf(
            if (Build.VERSION.SDK_INT >= 33)
                (ctx.checkSelfPermission(android.Manifest.permission.POST_NOTIFICATIONS) ==
                        android.content.pm.PackageManager.PERMISSION_GRANTED)
            else true
        )
    }
    var probeBusy by remember { mutableStateOf(false) }
    var probeResult by remember { mutableStateOf<RootChecker.ProbeResult?>(null) }

    fun refreshPerms() {
        overlayGranted = android.provider.Settings.canDrawOverlays(ctx)
        notifGranted = if (Build.VERSION.SDK_INT >= 33)
            (ctx.checkSelfPermission(android.Manifest.permission.POST_NOTIFICATIONS) ==
                    android.content.pm.PackageManager.PERMISSION_GRANTED)
        else true
    }

    DisposableEffect(Unit) { onDispose { refreshPerms() } }

    Column(
        Modifier
            .fillMaxSize()
            .padding(horizontal = 16.dp)
            .verticalScroll(rememberScrollState()),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        // ---- Permissions card ----
        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                Text("Permissions", style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.SemiBold)

                Row(
                    Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Text("Display over other apps (overlay)", style = MaterialTheme.typography.bodyMedium)
                    StatusChip(ok = overlayGranted)
                }
                Row(
                    Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Text("Notifications (Android 13+)", style = MaterialTheme.typography.bodyMedium)
                    StatusChip(ok = notifGranted)
                }
                Row(
                    Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    val mpOk = s.captureMode != "nonroot" || SessionManager.pendingProjectionData != null
                    Text("Screen capture (nonroot only)", style = MaterialTheme.typography.bodyMedium)
                    StatusChip(ok = mpOk, muted = s.captureMode != "nonroot")
                }

                Button(
                    onClick = {
                        refreshPerms()
                        Log.i("Config", "refresh permissions")
                    },
                    modifier = Modifier.fillMaxWidth(),
                    colors = ButtonDefaults.buttonColors(containerColor = accent, contentColor = Color(0xFF001418))
                ) { Text("Refresh permissions") }

                if (Build.VERSION.SDK_INT >= 23 && !overlayGranted) {
                    OutlinedButton(
                        onClick = {
                            try {
                                ctx.startActivity(
                                    Intent(Settings.ACTION_MANAGE_OVERLAY_PERMISSION)
                                        .setPackage(ctx.packageName)
                                        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                                )
                            } catch (t: Throwable) {
                                Log.e("Config", "open overlay settings: ${t.message}")
                            }
                        },
                        modifier = Modifier.fillMaxWidth()
                    ) { Text("Grant overlay permission") }
                }
            }
        }

        // ---- Capture mode card ----
        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                Text("Capture mode", style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.SemiBold)
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    FilterChip(selected = s.captureMode == "root",
                        onClick = { session.setCaptureMode("root") },
                        label = { Text("Root") })
                    FilterChip(selected = s.captureMode == "shizuku",
                        onClick = { session.setCaptureMode("shizuku") },
                        label = { Text("Shizuku") })
                    FilterChip(selected = s.captureMode == "nonroot",
                        onClick = { session.setCaptureMode("nonroot") },
                        label = { Text("Non-root") })
                }
                Text(
                    when (s.captureMode) {
                        "root" -> "Root screencap -> PNG in app files -> direct read. Highest performance. Needs a working root."
                        "shizuku" -> "Shizuku/shell screencap -> base64 read. Needs Shizuku running."
                        "nonroot" -> "MediaProjection (system consent dialog). No root needed."
                        else -> "mode: ${s.captureMode}"
                    },
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
        }

        // ---- Capture probe card ----
        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                Text("Capture self-test", style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.SemiBold)
                Text(
                    "Runs a real screencap round-trip to confirm the chosen mode can actually produce frames before a session starts.",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
                Button(
                    onClick = {
                        probeBusy = true
                        probeResult = null
                        Log.i("Config", "capture probe start (mode=${s.captureMode})")
                        // Run the probe off the UI thread, then post the result back.
                        runProbe(ctx) { r ->
                            probeResult = r
                            probeBusy = false
                        }
                    },
                    enabled = !probeBusy,
                    modifier = Modifier.fillMaxWidth(),
                    colors = ButtonDefaults.buttonColors(containerColor = accent, contentColor = Color(0xFF001418))
                ) { Text(if (probeBusy) "Testing..." else "Test capture now") }

                val r = probeResult
                if (r != null) {
                    Box(
                        Modifier
                            .fillMaxWidth()
                            .background(
                                if (r.ok) Color(0xFF0F2A1A) else Color(0xFF2A0F0F),
                                RoundedCornerShape(8.dp)
                            )
                            .padding(10.dp)
                    ) {
                        Text(
                            if (r.ok) "PASS" else "FAIL",
                            style = MaterialTheme.typography.titleSmall,
                            fontWeight = FontWeight.Bold,
                            color = if (r.ok) Color(0xFF4ADE80) else Color(0xFFF87171)
                        )
                    }
                    Text(
                        "${r.detail}  (${r.ms}ms, ${r.bytes}B)",
                        style = MaterialTheme.typography.bodySmall,
                        color = if (r.ok) Color(0xFF4ADE80) else Color(0xFFF87171)
                    )
                }
            }
        }

        // ---- Root status card ----
        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                    Text("Root / KernelSU", style = MaterialTheme.typography.titleMedium)
                    TextButton(onClick = { session.refreshRoot() }) { Text("refresh") }
                }
                Text(
                    "provider: ${s.rootInfo.provider}   ksu-next: ${if (s.rootInfo.ksuNext) "detected" else "n/a"}   uid: ${s.rootInfo.uid}",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
                Text(
                    if (s.rootInfo.root) "Root detected." else "No root detected (root mode may fail; try nonroot).",
                    style = MaterialTheme.typography.bodySmall,
                    color = if (s.rootInfo.root) Color(0xFF4ADE80) else Color(0xFFFBBF24)
                )
            }
        }

        // ---- Start card ----
        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                Text("Start", style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.SemiBold)
                val ready = overlayGranted && notifGranted &&
                        (s.captureMode != "nonroot" || SessionManager.pendingProjectionData != null)
                Button(
                    onClick = {
                        Log.i("Config", "Start pressed (ready=$ready)")
                        onStart()
                    },
                    enabled = ready && !session.isRunning(),
                    modifier = Modifier.fillMaxWidth(),
                    colors = ButtonDefaults.buttonColors(
                        containerColor = if (ready) Color(0xFF16A34A) else Color(0xFF334155),
                        contentColor = Color.White
                    )
                ) {
                    Text(if (session.isRunning()) "Running..." else "Start session")
                }
                if (!ready) {
                    Text(
                        "Grant the permissions above first (overlay + notifications${if (s.captureMode == "nonroot") " + screen capture" else ""}).",
                        style = MaterialTheme.typography.bodySmall,
                        color = Color(0xFFFBBF24)
                    )
                }
            }
        }

        Spacer(Modifier.height(24.dp))
    }
}

@Composable
private fun StatusChip(ok: Boolean, muted: Boolean = false) {
    val color = if (muted) Color(0xFF64748B) else (if (ok) Color(0xFF4ADE80) else Color(0xFFF87171))
    Box(
        Modifier
            .size(10.dp)
            .background(color, androidx.compose.foundation.shape.CircleShape)
    )
}

// Run a capture probe on a background thread and report the result on the UI thread.
private fun runProbe(ctx: Context, onDone: (RootChecker.ProbeResult) -> Unit) {
    Thread({
        val r = RootChecker.testCapture(ctx)
        Log.i("Config", "capture probe result: ok=${r.ok} detail=${r.detail}")
        android.os.Handler(android.os.Looper.getMainLooper()).post { onDone(r) }
    }, "lsfg-probe").apply { isDaemon = true; start() }
}
