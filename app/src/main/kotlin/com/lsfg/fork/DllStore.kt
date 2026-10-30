package com.lsfg.fork

import android.content.Context
import java.io.File

/**
 * Manages the user-supplied shader DLL (Lossless.dll / lsfg-vk.dll).
 *
 * Users can drop in their own compiled LSFG shader library via the file
 * picker; it is copied into the app's private files dir under a fixed name so
 * the native loader always has a stable path. "Reset" removes it and the
 * bundled `lsfg-vk.dll` asset is used instead.
 */
object DllStore {

    private const val USER_DLL_NAME = "user_shader.dll"
    private const val PREFS = "lsfg_prefs"
    private const val KEY_USER_DLL = "user_dll_active"

    fun userDllFile(ctx: Context): File = File(ctx.filesDir, USER_DLL_NAME)

    /** True if a user-supplied DLL is active. */
    fun isActive(ctx: Context): Boolean =
        ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getBoolean(KEY_USER_DLL, false)
                && userDllFile(ctx).exists()
                && userDllFile(ctx).length() > 0

    fun setActive(ctx: Context) {
        ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit()
            .putBoolean(KEY_USER_DLL, true).apply()
    }

    fun clear(ctx: Context) {
        ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit()
            .putBoolean(KEY_USER_DLL, false).apply()
        try { userDllFile(ctx).delete() } catch (_: Throwable) {}
    }

    /** Absolute path of the user DLL, or null to fall back to the bundled asset. */
    fun userDllPath(ctx: Context): String? {
        val f = userDllFile(ctx)
        return if (isActive(ctx)) f.absolutePath else null
    }
}
