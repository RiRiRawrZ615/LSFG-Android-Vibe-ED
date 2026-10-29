package com.lsfg.fork

import android.app.Application
import com.topjohnwu.superuser.Shell

class LSFGApp : Application() {
    override fun onCreate() {
        super.onCreate()
        try {
            // Prefer the root shell. Falls back to a non-root shell (uid 2000)
            // automatically if the root provider is unavailable.
            Shell.enableVerboseLogging = false
            Shell.setDefaultBuilder(
                // libsu 5.x: default builder tries root first, falls back to shell.
            Shell.Builder.create()
                    .setFlags(Shell.FLAG_REDIRECT_STDERR)
            )
        } catch (t: Throwable) {
            // Non-fatal: root features will just report unavailable
        }
    }
}
