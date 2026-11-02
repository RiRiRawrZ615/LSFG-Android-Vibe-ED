package com.lsfg.fork.session

/**
 * Logging facade for the ported LSFG-Android session/ capture skeleton.
 * Mirrors to logcat and the fork's on-device log file (com.lsfg.fork.Log)
 * so privileged-capture diagnostics survive a test session.
 */
object LsfgLog {

    fun i(tag: String, msg: String) {
        try {
            android.util.Log.i(tag, msg)
            com.lsfg.fork.Log.i(tag, msg)
        } catch (_: Throwable) {
            android.util.Log.i(tag, msg)
        }
    }

    fun w(tag: String, msg: String, tr: Throwable? = null) {
        try {
            if (tr != null) {
                android.util.Log.w(tag, msg, tr)
                com.lsfg.fork.Log.w(tag, "$msg ($tr)")
            } else {
                android.util.Log.w(tag, msg)
                com.lsfg.fork.Log.w(tag, msg)
            }
        } catch (_: Throwable) {
            if (tr != null) android.util.Log.w(tag, msg, tr) else android.util.Log.w(tag, msg)
        }
    }

    fun e(tag: String, msg: String, tr: Throwable? = null) {
        try {
            if (tr != null) {
                android.util.Log.e(tag, msg, tr)
                com.lsfg.fork.Log.e(tag, "$msg ($tr)")
            } else {
                android.util.Log.e(tag, msg)
                com.lsfg.fork.Log.e(tag, msg)
            }
        } catch (_: Throwable) {
            if (tr != null) android.util.Log.e(tag, msg, tr) else android.util.Log.e(tag, msg)
        }
    }
}
