package com.lsfg.fork

import android.app.ActivityManager
import android.content.Context
import android.content.Intent
import com.topjohnwu.superuser.Shell
import com.topjohnwu.superuser.ShellUtils

/**
 * Shell bridge: target app launch, foreground detection.
 * Root-based in v1 (scrcpy-style monkey + dumpsys parsing).
 */
object ShellBridge {

    /** All launchable apps, sorted by label. */
    fun launchableApps(context: Context): List<AppInfo> {
        val pm = context.packageManager
        val intent = Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER)
        val list = pm.queryIntentActivities(intent, 0)
        return list.mapNotNull { r ->
            AppInfo(pkg = r.activityInfo.packageName, label = r.loadLabel(pm).toString())
        }.sortedBy { it.label.lowercase() }
    }

    /** Launch a package via monkey (works from root shell, scrcpy style). */
    fun launch(pkg: String): Boolean = try {
        val out = Shell.cmd("monkey -p $pkg -c android.intent.category.LAUNCHER 1").exec()
        out.isSuccess
    } catch (_: Throwable) { false }

    /** Best-effort current foreground package. */
    fun foregroundPackage(context: Context): String? {
        try {
            val am = context.getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager
            val tasks = am.getRunningTasks(1)
            tasks.firstOrNull()?.topActivity?.packageName?.let { return it }
        } catch (_: Throwable) {
        }
        // Root fallback: dumpsys parsing
        try {
            val out = ShellUtils.fastCmd(
                "dumpsys activity activities | grep -E 'topResumedActivity|ResumedActivity' | head -3"
            )
            Regex("""[\w.]+/[\w.]+""").find(out)?.let { return it.value.substringBefore('/') }
            Regex("u0 ([\\w.]+)").find(out)?.let { return it.groupValues[1] }
        } catch (_: Throwable) {
        }
        return null
    }

    data class AppInfo(val pkg: String, val label: String)
}
