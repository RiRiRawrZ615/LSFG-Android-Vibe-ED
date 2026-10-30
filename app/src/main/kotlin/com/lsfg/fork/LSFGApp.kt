package com.lsfg.fork

import android.app.Application
import com.topjohnwu.superuser.Shell

class LSFGApp : Application() {

    companion object {
        @Volatile
        private var sAppContext: android.content.Context? = null

        fun appContext(): android.content.Context? = sAppContext
    }

    override fun onCreate() {
        super.onCreate()
        sAppContext = applicationContext

        // Initialize persistent on-device logging (bug 5).
        Log.init(applicationContext)
        Log.i("LSFGApp", "app start, minSdk=${android.os.Build.VERSION.SDK_INT}, " +
                "arch=${android.os.Build.SUPPORTED_ABIS.contentToString()}")

        try {
            // Prefer the root shell. Falls back to a non-root shell (uid 2000)
            // automatically if the root provider is unavailable.
            Shell.enableVerboseLogging = false
            Shell.setDefaultBuilder(
                Shell.Builder.create()
                        .setFlags(Shell.FLAG_REDIRECT_STDERR)
            )
            Log.i("LSFGApp", "libsu default shell configured")
        } catch (t: Throwable) {
            Log.w("LSFGApp", "libsu setup failed (non-fatal): ${t.message}")
        }
    }
}
