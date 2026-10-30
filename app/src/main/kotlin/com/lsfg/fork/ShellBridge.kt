package com.lsfg.fork

import android.app.ActivityManager
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import com.topjohnwu.superuser.Shell
import com.topjohnwu.superuser.ShellUtils

/**
 * Shell bridge: target app launch, foreground detection, Shizuku probe.
 * Root/shell-based in v1 (scrcpy-style monkey + dumpsys parsing).
 */
object ShellBridge {

    private const val SHIZUKU_PKG = "moe.shizuku.privileged.api"

    /** All launchable apps, sorted by label. */
    fun launchableApps(context: Context): List<AppInfo> {
        val pm = context.packageManager
        val intent = Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER)
        return pm.queryIntentActivities(intent, 0).mapNotNull { r ->
            AppInfo(pkg = r.activityInfo.packageName, label = r.loadLabel(pm).toString())
        }.sortedBy { it.label.lowercase() }
    }

    /** Launch a package via monkey (works from root/shell, scrcpy style). */
    fun launch(pkg: String): Boolean = try {
        val out = Shell.cmd("monkey -p $pkg -c android.intent.category.LAUNCHER 1").exec()
        out.isSuccess
    } catch (_: Throwable) { false }

    /** Is the Shizuku app installed? (Real availability needs the Shizuku SDK.) */
    fun shizukuInstalled(): Boolean {
        // Preferred: ask the PackageManager (works without root).
        LSFGApp.appContext()?.let { ctx ->
            return try {
                ctx.packageManager.getPackageInfo(SHIZUKU_PKG, 0) != null
            } catch (_: Throwable) { false }
        }
        // Fallback (shell):
        return try {
            val out = ShellUtils.fastCmd("pm list packages $SHIZUKU_PKG")
            out.contains("package:$SHIZUKU_PKG")
        } catch (_: Throwable) { false }
    }

    /** Is Shizuku currently running (privileged socket alive)? Best-effort. */
    fun shizukuRunning(): Boolean = try {
        // Shizuku's privileged helper keeps a unix socket under
        // /data/local/tmp/shizuku; a non-empty listing is a reasonable
        // heuristic without the Shizuku SDK.
        val out = ShellUtils.fastCmd("ls /data/local/tmp/shizuku")
        out.isNotBlank()
    } catch (_: Throwable) { false }

    /** Best-effort current foreground package. */
    fun foregroundPackage(context: Context): String? {
        try {
            @Suppress("DEPRECATION")
            val am = context.getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager
            val tasks = am.getRunningTasks(1)
            tasks.firstOrNull()?.topActivity?.packageName?.let { return it }
        } catch (_: Throwable) {}
        // Root fallback: dumpsys parsing.
        try {
            val out = ShellUtils.fastCmd(
                "dumpsys activity activities | grep -E 'topResumedActivity|ResumedActivity' | head -3"
            )
            Regex("""[\w.]+/[\w.]+""").find(out)?.let { return it.value.substringBefore('/') }
        } catch (_: Throwable) {}
        return null
    }

    data class AppInfo(val pkg: String, val label: String)
}
