package com.lsfg.fork

import android.os.Process
import com.topjohnwu.superuser.Shell
import com.topjohnwu.superuser.ShellUtils

/**
 * Root / KernelSU detection + capture verification.
 *
 * Two levels of confidence:
 *  1. [detect]  - "is root *available*?" (file/binary heuristics). Fast.
 *  2. [testCapture] - "does a *real* root screencap round-trip work?" This
 *     is what actually matters for capture mode and is what the config tab
 *     verifies before allowing a session to start (bug 2).
 */
object RootChecker {

    data class RootState(
        val root: Boolean,
        val provider: String,
        val ksuNext: Boolean,
        val uid: Int
    )

    data class ProbeResult(
        val ok: Boolean,
        val detail: String,
        val bytes: Long,
        val ms: Long
    )

    private val rootBinaries = arrayOf(
        "/system/bin/su", "/system/xbin/su", "/data/local/bin/su",
        "/data/local/xbin/su", "/sbin/su", "/system/bin/su", "/vendor/bin/su"
    )

    /** Fast heuristic: do root binaries / markers exist? (does NOT prove capture works) */
    private fun heuristicsRoot(): Boolean {
        for (p in rootBinaries) {
            val f = java.io.File(p)
            if (f.exists() && f.canExecute()) return true
        }
        if (java.io.File("/dev/ksu").exists()) return true
        if (java.io.File("/sbin/.magisk").exists()) return true
        if (Process.myUid() == 0) return true
        return false
    }

    fun detect(): RootState {
        var provider = "none"
        var shellRoot = false
        try {
            shellRoot = Shell.rootAccess()
            provider = if (shellRoot) "root" else "shell"
        } catch (_: Throwable) {
            provider = "none"
        }

        var ksu = false
        try {
            val out = ShellUtils.fastCmd("command -v ksu 2>/dev/null").trim()
            if (out.contains("ksu", ignoreCase = true)) ksu = true
        } catch (_: Throwable) {}

        val root = shellRoot || heuristicsRoot()
        return RootState(
            root = root,
            provider = provider,
            ksuNext = ksu,
            uid = Process.myUid()
        )
    }

    /**
     * Run a real `screencap` to a private file, read it back, and measure
     * the round-trip. This is the authoritative check that the chosen
     * capture path actually produces frames (bug 6).
     *
     * @return ProbeResult.ok == true only if a non-empty PNG was captured
     *         and read back successfully.
     */
    fun testCapture(context: android.content.Context): ProbeResult {
        val t0 = System.currentTimeMillis()
        val path = "/data/local/tmp/lsfg_root_test_${System.nanoTime()}.png"
        return try {
            // screencap writes a PNG; the default shell (root if available,
            // else uid 2000) can write to /data/local/tmp and read it back.
            val res = Shell.cmd("screencap -p $path").exec()
            val ms = System.currentTimeMillis() - t0
            val errText = res.getErr().joinToString(" ").trim()
            if (!res.isSuccess) {
                cleanup(path)
                return ProbeResult(false, "screencap exited ${res.getCode()}: $errText", 0, ms)
            }
            // Confirm the file is non-empty by reading its byte size via shell.
            val size = try {
                val out = ShellUtils.fastCmd("wc -c < $path").trim()
                out.toLongOrNull() ?: 0L
            } catch (_: Throwable) { 0L }
            if (size < 64L) {
                cleanup(path)
                return ProbeResult(false, "screencap produced too-small file (${size}B)", size, ms)
            }
            cleanup(path)
            ProbeResult(true, "screencap OK (${size}B in ${ms}ms)", size, ms)
        } catch (t: Throwable) {
            cleanup(path)
            ProbeResult(false, "probe threw: ${t.message}", 0, System.currentTimeMillis() - t0)
        }
    }

    /** Best-effort foreground package (for the config tab "launch target"). */
    fun shizukuAvailable(): Boolean = try {
        // Shizuku runs a privileged socket; a cheap heuristic is the presence
        // of its app + a listening port. Real check requires the Shizuku SDK.
        val out = ShellUtils.fastCmd("ls /data/local/tmp/shizuku 2>/dev/null | wc -l").trim()
        (out.toIntOrNull() ?: 0) >= 1 ||
            com.lsfg.fork.ShellBridge.shizukuInstalled()
    } catch (_: Throwable) {
        false
    }

    private fun cleanup(path: String) {
        try { ShellUtils.fastCmd("rm -f $path") } catch (_: Throwable) {}
    }
}
