/*
 * jnifile_log.h - forward native log lines to the app's on-device log file.
 *
 * The C++ framegen and WSI libraries log to logcat only (tag "lsfg-framegen" /
 * "lsfg-wsi"). The user's exported on-device log file only captured Kotlin
 * lines, so the REAL reason a failure happened (a C++ exception, a Vulkan error
 * code, a createImage failure) was invisible in the exported file. This tiny
 * inline helper lets native code also call com.lsfg.fork.Log.nativeLog(...) so
 * those lines are persisted to the same file the user exports.
 *
 * Usage (each shared object that wants this):
 *   #include "jnifile_log.h"
 *   ... in a log macro: lsfgjlog::log(ANDROID_LOG_ERROR, "lsfg-framegen", msg);
 *   and:
 *   extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
 *       JNIEnv* env = nullptr;
 *       if (vm->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_OK)
 *           lsfgjlog::init(env, vm);
 *       return JNI_VERSION_1_6;
 *   }
 *
 * Safe to call from worker threads (attaches/detaches as needed). Never blocks
 * the pipeline for more than the cost of a JNI call + a bounded queue offer.
 */
#pragma once

#include <jni.h>
#include <android/log.h>
#include <string>
#include <cstdarg>
#include <cstdio>

namespace lsfgjlog {

    inline JavaVM* g_vm = nullptr;
    inline jclass g_cls = nullptr;      // global ref to com/lsfg/fork/Log
    inline jmethodID g_mid = nullptr;   // static nativeLog(int, String, String)

    // Call from JNI_OnLoad (main thread, valid env). Caches the JavaVM and the
    // static method so worker threads don't re-resolve it.
    inline void init(JNIEnv* env, JavaVM* vm) {
        g_vm = vm;
        if (!g_cls) {
            jclass c = env->FindClass("com/lsfg/fork/Log");
            if (c) {
                g_cls = (jclass)env->NewGlobalRef(c);
                g_mid = env->GetStaticMethodID(
                    g_cls, "nativeLog",
                    "(ILjava/lang/String;Ljava/lang/String;)V");
            }
        }
    }

    // androidLevel: ANDROID_LOG_* (1=V,2=D,3=I,4=W,5=E,6=F).
    inline void log(int androidLevel, const char* tag, const char* msg) {
        if (!g_vm || !g_cls || !g_mid || !tag || !msg) return;
        JNIEnv* env = nullptr;
        bool attached = false;
        if (g_vm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
            if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
            attached = true;
        }
        // Map ANDROID_LOG_* (1..6) to the app's Level code (2=D,3=I,4=W,5=E).
        int appLevel = androidLevel;
        if (androidLevel <= 2) appLevel = 2;
        else if (androidLevel >= 6) appLevel = 5;
        else appLevel = androidLevel; // 3,4,5 pass through
        jstring jtag = env->NewStringUTF(tag);
        jstring jmsg = env->NewStringUTF(msg);
        env->CallStaticVoidMethod(g_cls, g_mid, appLevel, jtag, jmsg);
        env->DeleteLocalRef(jtag);
        env->DeleteLocalRef(jmsg);
        if (attached) g_vm->DetachCurrentThread();
    }


    // Format (like printf) then log to logcat + on-device file. Safe on worker
    // threads. Fixed 4KB buffer; longer messages are truncated.
    inline void logprintf(int androidLevel, const char* tag, const char* fmt, ...) {
        char buf[4096];
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        if (n < 0) buf[0]=0;
        __android_log_print(androidLevel, tag, "%s", buf);
        log(androidLevel, tag, buf);
    }

} // namespace lsfgjlog
