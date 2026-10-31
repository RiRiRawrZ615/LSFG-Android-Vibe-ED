package com.lsfg.fork.ui

import android.content.Intent
import android.net.Uri
import androidx.core.content.FileProvider
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.foundation.layout.height
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.lsfg.fork.Log

/**
 * Logs tab (bug 5): view the on-device log file, refresh it, and export it
 * to Downloads for sharing / diagnosis.
 */
@Composable
fun LogsScreen() {
    val ctx = LocalContext.current
    val accent = Color(0xFF22D3EE)
    var logText by remember { mutableStateOf("") }
    var filePath by remember { mutableStateOf("") }
    var fileSize by remember { mutableLongStateOf(0L) }
    var shareUri by remember { mutableStateOf<Uri?>(null) }
    var exporting by remember { mutableStateOf(false) }

    fun refresh() {
        val f = Log.currentFile(ctx)
        filePath = f.absolutePath
        fileSize = f.length()
        logText = Log.tail(ctx, 48 * 1024)
    }

    LaunchedEffect(Unit) { refresh() }

    Column(
        Modifier
            .fillMaxSize()
            .padding(horizontal = 16.dp)
            .verticalScroll(rememberScrollState()),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                Text("Logs", style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.SemiBold)
                Text(
                    "Every session action, permission check, capture error, and framegen init is written to " +
                    "this file. Use it to diagnose why a session isn't working.",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                    Text(
                        if (filePath.isNotEmpty()) filePath else "(no log file yet)",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                    Text(
                        if (fileSize > 0) "${fileSize / 1024} KB" else "-",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }
                Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                    Button(onClick = {
                        Log.i("Logs", "user refreshed log view")
                        refresh()
                    }, modifier = Modifier.weight(1f),
                        colors = ButtonDefaults.buttonColors(containerColor = accent, contentColor = Color(0xFF001418))) {
                        Text("Refresh")
                    }
                    Button(onClick = {
                        exporting = true
                        Thread({
                            val path = Log.exportToDownloads(ctx)
                            val handler = android.os.Handler(android.os.Looper.getMainLooper())
                            handler.post {
                                exporting = false
                                if (path != null) {
                                    // CRITICAL FIX (bug: "logs don't export to
                                    // downloads / share crashes"): the old code
                                    // used Uri.fromFile() -> a file:// URI.
                                    // Sharing a file:// URI on API 24+ throws
                                    // FileUriExposedException ("exposed beyond
                                    // app through ClipData"). Use FileProvider so
                                    // the share target receives a content:// URI
                                    // with read permission granted. If the export
                                    // already produced a MediaStore content://
                                    // URI string, use it as-is.
                                    shareUri = if (path.startsWith("content:")) {
                                        Uri.parse(path)
                                    } else {
                                        FileProvider.getUriForFile(
                                            ctx,
                                            "${ctx.packageName}.files",
                                            java.io.File(path)
                                        )
                                    }
                                }
                            }
                        }, "lsfg-export").apply { isDaemon = true; start() }
                    }, enabled = !exporting, modifier = Modifier.weight(1f)) {
                        Text(if (exporting) "Exporting..." else "Export to Downloads")
                    }
                }
                val u = shareUri
                if (u != null) {
                    OutlinedButton(onClick = {
                        try {
                            val share = Intent(Intent.ACTION_SEND).apply {
                                type = "text/plain"
                                putExtra(Intent.EXTRA_STREAM, u)
                                addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
                                addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                            }
                            ctx.startActivity(Intent.createChooser(share, "Share LSFG log"))
                        } catch (t: Throwable) {
                            Log.e("Logs", "share failed: ${t.message}")
                        }
                    }, modifier = Modifier.fillMaxWidth()) {
                        Text("Share log file")
                    }
                }
            }
        }

        // Log tail (read-only, monospace, newest at bottom).
        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(14.dp)) {
                Text("Log tail (newest last)", style = MaterialTheme.typography.titleSmall,
                    fontWeight = FontWeight.SemiBold)
                Spacer(Modifier.height(8.dp))
                Box(
                    Modifier
                        .fillMaxWidth()
                        .height(360.dp)
                        .background(Color(0xFF0B0F14), RoundedCornerShape(8.dp))
                        .padding(10.dp)
                ) {
                    Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState())) {
                        val lines = logText.ifBlank { "(empty - start a session to generate logs)" }.lines()
                        // Show last ~200 lines for readability.
                        val shown = if (lines.size > 200) lines.takeLast(200) else lines
                        shown.forEach { ln ->
                            Text(
                                ln,
                                fontFamily = FontFamily.Monospace,
                                fontSize = 11.sp,
                                color = logColor(ln),
                                softWrap = false,
                                overflow = androidx.compose.ui.text.style.TextOverflow.Ellipsis,
                                maxLines = 1
                            )
                        }
                    }
                }
            }
        }

        Spacer(Modifier.height(24.dp))
    }
}

private fun logColor(line: String): Color {
    return when {
        line.contains(" E ", true) -> Color(0xFFF87171)
        line.contains(" W ", true) -> Color(0xFFFBBF24)
        line.contains(" I ", true) -> Color(0xFF4ADE80)
        else -> Color(0xFF94A3B8)
    }
}
