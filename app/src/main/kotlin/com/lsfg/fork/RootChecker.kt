package com.lsfg.fork

import android.os.Process
import com.topjohnwu.superuser.Shell
import com.topjohnwu.superuser.ShellUtils

/** Root / KernelSU detection for the UI status line. */
object RootChecker {

    /** Fast synchronous check without blocking on shell init. */
    fun detectFast(): Boolean {
        // Check for root binaries that indicate root is available
        val rootPaths = arrayOf("/system/bin/su", "/system/xbin/su", "/data/local/bin/su", "/data/local/xbin/su", "/sbin/su")
        for (p in rootPaths) {
            val f = java.io.File(p)
            if (f.exists() && f.canExecute()) return true
        }
        // Check KernelSU
        if (java.io.File("/dev/ksu").exists()) return true
        if (java.io.File("/sbin/.magisk").exists()) return true
        // Check uid
        if (Process.myUid() == 0) return true
        return false
    }

    fun detect(): RootState {
        var root = detectFast()
        var provider = "none"
        try {
            root = Shell.rootAccess()
            provider = if (root) "root" else "shell"
        } catch (_: Throwable) {
            // If shell failed, keep fast-check result
            provider = "none"
        }

        // Probe KernelSU Next: ksu binary / procfs hints.
        var ksu = false
        try {
            val out = ShellUtils.fastCmd("command -v ksu 2>/dev/null; ls /mnt/k 2>/dev/null | head -1").trim()
            if (out.isNotBlank() && out.contains("ksu", ignoreCase = true)) ksu = true
            if (!ksu) {
                val maps = ShellUtils.fastCmd("cat /proc/self/maps 2>/dev/null | grep -c ksu").trim()
                ksu = (maps.toIntOrNull() ?: 0) > 0
            }
        } catch (_: Throwable) {
        }

        return RootState(root = root || detectFast(), provider = provider, ksuNext = ksu, uid = Process.myUid())
    }

    /** Detect Shizuku via its service socket. */
    fun shizukuAvailable(): Boolean = try {
        val out = ShellUtils.fastCmd("ls /data/local/tmp/shizuku 2>/dev/null | wc -l").trim()
        out.toIntOrNull() == 1
    } catch (_: Throwable) {
        false
    }

    data class RootState(
        val root: Boolean,
        val provider: String,
        val ksuNext: Boolean,
        val uid: Int
    )
}
