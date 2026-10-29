package com.lsfg.fork

import android.os.Process
import com.topjohnwu.superuser.Shell
import com.topjohnwu.superuser.ShellUtils

/** Root / KernelSU detection for the UI status line. */
object RootChecker {

    fun detect(): RootState {
        var root: Boolean = false
        var provider = "none"
        try {
            root = Shell.rootAccess()
            provider = if (root) "root" else "shell"
        } catch (_: Throwable) {
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

        return RootState(root = root, provider = provider, ksuNext = ksu, uid = Process.myUid())
    }

    data class RootState(
        val root: Boolean,
        val provider: String,
        val ksuNext: Boolean,
        val uid: Int
    )
}
