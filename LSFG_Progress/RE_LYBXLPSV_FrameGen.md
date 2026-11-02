# RE of lybxlpsv "Frame Generation" (lybfgapp v0.5.48 + lybfghook Zygisk v0.6.3701)

Date: 2026-11-01. Tools: jadx 1.5.1, apktool 2.11.1, radare2 5.9.8, readelf/nm/strings.
Sources: /home/amd/Documents/AI_Output/ToReverse/{lybfgapp_v0.5.48.apk, lybfghook-v0.6.3701_public-release.zip}
Working copy: /home/amd/Documents/AI_Output/re/ (apk/, zygisk/, zy/native/). Decompiled DEX: /home/amd/Documents/AI_Output/re/apk/jadx_src/.

## 0. What this app actually is (corrects the naive read)

It is NOT a single "framegen app + root module". It is a three-part system, and the
frame-generation compute is NOT where you'd expect:

  component                     what it is                                          where the framegen work lives
  ----------------------------  ---------------------------------------------------  --------------------------------------
  lybfgapp (APK)                Kotlin/Compose UI + license + driver probe            NO compute. UI, licensing, GPU/driver probe, launches daemon
  lybfghook (Zygisk module)     inline-hooking .so injected into the GAME process    EGL/GLESv2+v3 hooking. NO Vulkan, NO embedded SPIR-V
  lybfgdaemon (assets/ PIE)     root userspace GPU/display scheduler                  NO compute. KGSL/Mali clock pin, cpuset, SurfaceFlinger

The real "lossless scaling frame gen" is the well-known LSFG v3_1 compute pipeline
(alpha/beta/gamma/delta/epsilon/generate + mipmap prepass). lybxlpsv bundles a Vulkan
loader (libframegen.so dlopens libvulkan.so at runtime) and the hook drives it inside the
target process. The closed-source value is the HOOK (how it injects, intercepts the
game's present path, and feeds frames into the pipeline) plus the DAEMON (how it forces
the GPU/panel to uncapped high refresh). Both are described below.

## 1. APK: lybfgapp_v0.5.48.apk
- R8-obfuscated (single-letter packages a/ b0/ c2/ ... j7/ j8/ r0/ y8/). App package: com.lybxlpsv.framegen.
- Named (unobfuscated) classes: MainActivity, SplashActivity, AppLink, webview, ForegroundServiceTextOnly, BootReceiver, framegen$Companion (JNI).
- lib/arm64-v8a/libframegen.so (354KB, NDK r25b, stripped):
    NEEDED: libandroid, libGLESv3, libEGL, liblog, libm, libdl, libc. (NOT linked to libvulkan; dlopens it at runtime.)
    JNI exports (com.lybxlpsv.framegen.framegen$Companion):
        long   Fg(String)            -> opaque handle (init/start the fg engine, arg = config)
        long   Fgx(String)
        String GlDumpStr(); int VkDump(); String VkDumpStr();   // EGL/Vulkan driver string dumps
        int    getGPUMajor(); String getGPU();
        int    getVKDrvMajor/Minor/Build()
        String run_bench()
        boolean verifgSig(byte[], byte[])   // license signature check (data + sig) -> k5.Valid/k5.Invalid
    => libframegen.so is a THIN JNI driver-probe + engine bootstrap. It probes GPU/driver and exposes the Vulkan engine, but the heavy lifting is in the hook.
- assets/main.jar (3.6KB): a tiny secondary DEX (probe helper).
- assets/lybfgdaemon (568KB PIE): the root scheduler (see section 3).
- DEX behavior strings (mined across all decompiled code):
    Root/Zygisk/KernelSU detection:
        /data/adb/modules/zygisk_shamiko/module.prop
        /data/adb/modules/rezygisk/module.prop
        /data/adb/modules/treat_wheel/module.prop
        /data/adb/modules/vulkanhook/module.prop
        /data/adb/modules/zygisksu/module.prop
        (KernelSU) "lybkm"  module
        "lybfgdaemon"  /  "lybfg_daemon"  /  "lybfgdaemononboot"
    Display/refresh:
        "service call SurfaceFlinger "   (binder transaction to uncap/force refresh)
        "surfaceflinger", "thermal_limit_refresh_rate", kgsl_gpu_{block,min,max,boost,usg_up,usg_down}
    => The app: detects a compatible root env, installs/verifies the Zygisk+KernelSU modules,
       starts lybfgdaemon, probes GPU/driver via libframegen, verifies license (verifgSig),
       then selects a target app and issues start/stop to the hook.

## 2. Zygisk module: lybfghook v0.6.3701 (THE closed-source crown jewel)
- module.prop: id/name=lybfghook, version=v0.6.3701_public, versionCode=1, author=lybxlpsv, desc="lybxlpsv's fg hook".
- lyb.prop:  fgappver=7 , vulkan=2    (protocol/version handshake with the app; vulkan=2 = Vulkan path)
- module.prop.sig / lyb.prop.sig / *.so.sig: 64-byte signatures (ECDSA-SHA256, 64B = r||s) to pin module<->app version and prevent tamper.
- META-INF/com/google/android/update-binary: standard Magisk>=20.4 util_functions install_module.
- eula.html / license.txt: proprietary EULA ("one license per device", "no tampering or redistribution", as-is warranty) + Press Start 2P (OFL) font + Khronos headers.
- zygisk/arm64-v8a.so (6.41MB) :  SONAME liblybfghook.so
    NEEDED: liblog, libGLESv3, libGLESv2, libEGL, libm, libdl, libc   (EGL/GLES, NO Vulkan link, NO AHardwareBuffer/gralloc link)
    exports: zygisk_module_entry, zygisk_companion_entry   (standard Zygisk ABI)
    Embedded (statically linked): OpenSSL 3.x (full: ASN1/EVP/RSA/ECDH/ECDSA + QUIC/HTTP3 + ossl_quic_* + x509) -- used for the license/verifgSig crypto and/or a license server over TLS/QUIC.
    Inline-hooking engine present: string "25FunctionInlineHookRouting" (a hook-routing table class), plus generic PLT/text hooking.
    NEGATIVE results (important):
        SPIR-V magic 0302 2307 count = 0      => shaders are NOT embedded in the hook
        "vulkan","libvulkan.so","vkCreateInstance" count = 0
        "eglSwapBuffers","eglCreateContext","eglMakeCurrent" count = 0  (EGL is dlsym'd at runtime, not linked by name in .rodata)
        "AHardwareBuffer","ANativeWindow","gralloc1" count = 0
    => The hook hooks the GAME's EGL/GLES present path (eglSwapBuffers / eglCreateContext / ANativeWindow_queueBuffer via dlsym + inline hooks), grabs the rendered buffer, and hands it to the framegen engine. It does NOT itself create a Vulkan device. The Vulkan compute (libframegen.so / libvulkan.so) is a separate component it drives. This is why "it insists on root": the hook must be injected into the game's own process (zygote fork) to reach its live EGL buffers without a capture pass.

  Practical consequence for the fork: the hard, proprietary piece is the in-process inline hook that intercepts the game's EGL present and feeds AHardwareBuffers into the LSFG pipeline. Our fork currently captures via screencap/MediaProjection/Shizuku (out-of-process). Matching lybxlpsv means reimplementing that in-process EGL hook as a Zygisk module, OR accepting out-of-process capture (lower latency floor, no root required).

## 3. assets/lybfgdaemon (568KB PIE) -- the stealable, self-contained root scheduler
- NEEDED: only libm, libdl, libc (everything else via dlopen; NDK r29, PGO+BOLT+LTO, stripped). No exported symbols -> internal main + self-contained.
- Behavior strings (full inventory captured; the stealable set):
    GPU clock / boost (Adreno/KGSL):
        /dev/kgsl-3d0
        /sys/class/kgsl/kgsl-3d0/min_pwrlevel
        /sys/class/kgsl/kgsl-3d0/max_pwrlevel
        /sys/class/kgsl/kgsl-3d0/min_clock_mhz
        /sys/class/kgsl/kgsl-3d0/max_clock_mhz
        /sys/class/kgsl/kgsl-3d0/clock_mhz
        /sys/class/kgsl/kgsl-3d0/gpu_available_frequencies
        /sys/class/kgsl/kgsl-3d0/devfreq/min_freq
        /sys/class/kgsl/kgsl-3d0/devfreq/max_freq
        /sys/class/kgsl/kgsl-3d0/gpubusy
        kgsl_gpu_block / kgsl_gpu_min / kgsl_gpu_max / kgsl_gpu_boost / kgsl_usg_up / kgsl_usg_down
        "Kgsl new gpu freq = "  /  "Kgsl devfreq write failed"  /  "Kgsl work done, sleeping mode"
    Mali:
        /dev/mali0        "GPU Type: Mali"
    CPU pinning:
        /dev/cpuset/top-app/cpus      (pin target to the "top-app" cpuset = big cores, no migration)
    Panel/refresh uncapping (the key "insist on root" value):
        "service call SurfaceFlinger "    (raw binder: force display refresh / disable thermal cap)
        "settings put system thermal_limit_refresh_rate 1"   (disable MIUI/thermal refresh clamp)
        "daemon_miui_uncap"               (MIUI-specific uncap routine)
    State/telemetry files:
        /data/vendor/gpu/glfg.d     (control/lock dir)
        /data/vendor/gpu/glfg.txt   (status/telemetry text)
    Kill switch:
        /data/local/tmp/disable_lybfgdaemon   and   /sdcard/disable_lybfgdaemon
    Thread model:
        "[CPU_THREAD] initialization failed!"   (dedicated CPU pinning/monitor thread)
        "Exception, restarting..."  (resilient loop)
- What it does (inferred, high confidence): runs as root (launched by the app or on boot via lybfgdaemononboot), finds the foreground game, pins it to the top-app cpuset, forces the Adreno/Mali to max clock and pins it (min=max=max clock, devfreq locked, kgsl boost), disables the thermal refresh clamp, issues a SurfaceFlinger binder transaction to force/hold high refresh, and writes glfg.txt telemetry. This is what removes the OS 60fps cap that the website says breaks rootless.

## 4. Steal-into-fork plan (concrete)
Value-ranked, each independently buildable:
 A. [HIGH, self-contained] Port lybfgdaemon's uncapping into the fork's ROOT capture path as a
    Kotlin ShellBridge routine + (optionally) a small native helper:
        - Adreno: write kgsl-3d0 min/max clock + pwrlevel to max, devfreq min=max, kgsl boost; poll gpubusy.
        - Mali: /dev/mali0 clock pin where available.
        - cpuset: echo target pid/cgroup to /dev/cpuset/top-app.
        - refresh: `settings put system thermal_limit_refresh_rate 1` + the SurfaceFlinger service call.
        - kill switch: honor /data/local/tmp/disable_lsfg_... ; write lsfg fg status file.
    This is the single most impactful thing to "steal", is fully reconstructable from the strings above,
    and directly fixes our root path's 60fps-cap problem. No root-only deps beyond what our Root capture engine already has.
 B. [MED] Reimplement verifgSig-style license check + module<->app version handshake (fgappver/vulkan)
    so our fork can ship an optional Zygisk module later without a closed handshake.
 C. [MED, big] Reimplement the in-process EGL hook as a Zygisk module to match latency. This is the
    large project: Zygisk module_entry + dlsym egl* + inline hook of eglSwapBuffers/queueBuffer,
    extract the presented buffer as AHardwareBuffer, feed our existing liblsfg_framegen.so pipeline,
    present the generated frame back. Reuses our existing framegen + WSI code; only the hook layer is new.
 D. [LOW] Driver probe (getGPUMajor/getVKDrv*/run_bench) already effectively present in our fork's
    root probe; align naming for parity.

## 5. Build/test state of the fork (verified 2026-11-01)
- /home/amd/Documents/AI_Output/Forks/LSFG-Fork builds: BUILD SUCCESSFUL (git HEAD v1.7). APK: LSFG-Fork-v1.8-2026-11-01_1635Z.apk.
- "Broken" = runtime (framegen/capture wiring), not the build. Known TODOs in work.log: wire framegen end-to-end on device, Shizuku path, raw-RGBA capture, zero-copy AHB.
- Emulator: ~/Android/Sdk, AVD lsfg34 (Android 14 / API 34 / x86_64), KVM present, device emulator-5554 boot_completed. SwiftShader Vulkan (no real Adreno) -> KGSL uncapping is structural there; Adreno paths are exercised on the real device.

## 6. Open questions (need real device or frida)
- Exact SurfaceFlinger service-call args (code, offsets) for the uncap transaction. (Readable from daemon disasm, but PGO/BOLT makes it noisy; confirm on device via logcat/strace.)
- Whether the hook feeds frames via AHardwareBuffer or via a GL texture/eglImage to the Vulkan engine (needs frida on the game process).
- Exact verifgSig curve/params (ECDSA P-256 r||s assumed from 64-byte sig).
