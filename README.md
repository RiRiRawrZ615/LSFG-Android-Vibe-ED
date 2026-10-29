# LSFG-Android-Vibe-ED

> ## ⚠️ WORK IN PROGRESS ⚠️
> **Early, experimental Android port. It compiles to an APK but is NOT finished and NOT recommended for daily use.**
> Core frame-generation is not fully wired end-to-end, some capture paths are unimplemented, and on-device testing is incomplete. Expect rough edges, bugs, and things that simply don't work yet. Do not treat this as production-ready.

**LSFG** (Lossless Scaling **Frame Gen**) is a real-time, Vulkan-based frame interpolation + upscaling pipeline that sits between the running application and the display to produce additional, smooth, high-quality interpolated frames. This repository is an **Android port** of the `lsfg-vk` compute pipeline: a native C++ Vulkan frame-generation engine, a Vulkan WSI presenter for an overlay, and a brand-new **Kotlin + Jetpack Compose (Material You)** front-end.

> ## 🤖 Vibe-Coded by Qwen
> This entire project was **vibe-coded** — prompt-driven, iterative, "feel it out as we go" — with **Qwen** (`Qwen3.8-27B-Heretic`) as the code-driving AI and **RiRi** as the human steering the wheel. It is an experimental "how far can we push a PC frame-gen pipeline onto Android" effort, not a polished product.

---

## What it does (by design)
- Captures the screen of a running (game) app on an Android device.
- Feeds frames into a native Vulkan **compute pipeline** (LSFG `v3_1` / `LSFG_3_1P` signature) that generates extra interpolated frames and upscales the result.
- Presents the generated frames to an **overlay window** via a Vulkan WSI swapchain (vsync-paced).
- Drives everything through a fresh **Material You** UI: capture-mode picker, frame multiplier, flow scale, performance mode, session start/stop, root status.

### Two native libraries
| Library | Responsibility |
|---|---|
| `liblsfg_framegen.so` | The Vulkan compute pipeline: shader loader (from `lsfg-vk.dll`), `v3_1` signature (built `consteval` at compile time), `dispatch`/`acquire`/`idle` frame loop, and `AHardwareBuffer` import (source/dest images). |
| `liblsfg_wsi.so` | Overlay rendering: Android surface + swapchain, blit of generated frames to the swapchain image, vsync-paced present. |

### Capture modes (planned / in progress)
1. **MediaProjection** (rootless, per-session consent dialog) — *TODO*
2. **Shizuku** (privileged shell, `ScreenCapture` API) — *TODO*
3. **Root** (root shell `screencap` → decode → `AHardwareBuffer`) — *basic path implemented*

---

## Status / known TODOs
- ✅ Builds to a debug APK (`arm64-v8a` + `x86_64`)
- ⏳ On-device testing (capture + overlay presentation)
- ⏳ Framegen pipeline currently in **passthrough** mode; full `v3_1` generation not yet wired end-to-end
- ⏳ Shizuku capture path
- ⏳ MediaProjection (rootless) fallback
- ⏳ UI polish (Material You)
- ⏳ Capture optimization (raw RGBA, zero-copy `AHardwareBuffer`)

## Build
Requires the Android SDK (platform `35`, build-tools `35`, NDK `27.0.12077973`, CMake `3.22.1`) and JDK 21.

```bash
./gradlew :app:assembleDebug
# APK -> app/build/outputs/apk/debug/app-debug.apk
```
Target ABIs: `arm64-v8a` (device), `x86_64` (emulator).

## Project layout
```
app/
├── build.gradle.kts
└── src/main/
    ├── AndroidManifest.xml
    ├── assets/lsfg-vk.dll              # shader library (PE32+ resources)
    ├── cpp/
    │   ├── CMakeLists.txt
    │   ├── framegen/                    # ported lsfg-vk compute core
    │   │   ├── pipelines/v3_1.*         # signature + pass list
    │   │   ├── library.* / dll.*        # shader DLL loading
    │   │   ├── signature/               # consteval pipeline signature builder
    │   │   ├── shaders/Mipmaps.h        # precompiled SPIR-V
    │   │   ├── thirdparty/              # vulkan + vk_video headers, vk shim
    │   │   ├── vkhelper.*              # Vulkan helper (image/ahb create, etc.)
    │   │   └── lsfg_jni.cpp             # JNI bridge (FramegenBridge)
    │   ├── lsfg_display.{h,cpp}         # frame queue: framegen -> wsi
    │   └── wsi/                         # volk + Android surface presenter
    ├── kotlin/com/lsfg/fork/            # Compose app + native bridges
    │   ├── LSFGApp.kt / MainActivity.kt / OverlayActivity.kt
    │   ├── capture/CaptureEngine.kt
    │   ├── framegen/FramegenBridge.kt
    │   ├── wsi/WsiBridge.kt
    │   ├── ShellBridge.kt / SessionManager.kt / RootChecker.kt
    │   └── ui/HomeScreen.kt / Theme.kt
    └── res/                             # Material theme + launcher icon
LSFG_Progress/                           # research findings + work log (for future agents)
```

## Attribution & upstream
- Vulkan frame-generation pipeline derived from the **lsfg-vk** project (Lossless Scaling / LSFG frame generation); shader resources come from its `lsfg-vk.dll` / `Lossless.dll`.
- Reference Android implementations studied during the port: `LSFG-Android`, `scrcpy` (VirtualDisplay capture), and `Shizuku` (ScreenCapture API).
- Vulkan bindings/headers: `Vulkan-Headers` / `vk_video`, plus `volk`.

## License
TBD — pending upstream license confirmation before redistribution. Respect and check the upstream `lsfg-vk` project license and the licenses of any bundled headers.

---
*Vibe-coded by Qwen · Experimental · Not yet ready for production · Use at your own risk.*
