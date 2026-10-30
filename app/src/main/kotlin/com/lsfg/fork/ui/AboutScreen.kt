package com.lsfg.fork.ui

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import com.lsfg.fork.BuildConfig

/**
 * Attributions tab (bug 3): credit the official upstream sources this
 * implementation is derived from, plus the open-source dependencies.
 */
@Composable
fun AboutScreen() {
    Column(
        Modifier
            .fillMaxSize()
            .padding(horizontal = 16.dp)
            .verticalScroll(rememberScrollState()),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("LSFG-Fork", style = MaterialTheme.typography.titleLarge, fontWeight = FontWeight.Bold,
                    color = Color(0xFF22D3EE))
                Text("frame interpolation for Android", style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant)
                Text(
                    "version ${BuildConfig.VERSION_NAME} (code ${BuildConfig.VERSION_CODE})  |  minSdk ${29}",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
                Text(
                    "A from-scratch Android port of Lossless Scaling Frame Generation (LSFG) that " +
                    "generates interpolated frames on-device via a Vulkan compute pipeline, captures the " +
                    "screen (root / Shizuku / MediaProjection), and presents the result on a touch-" +
                    "passing overlay.",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurface
                )
            }
        }

        // ---- Primary upstream ----
        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                Text("Based on", style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.SemiBold)
                AttributionRow(
                    name = "LSFG-vk (Lossless Scaling Frame Generation)",
                    desc = "The frame-generation pipeline (v3.1 signature, compute passes, shader " +
                            "extraction) is a port of the lsfg-vk Vulkan layer, developed primarily by " +
                            "PancakeTAS. It brings Lossless Scaling's frame generation to Vulkan " +
                            "applications. The bundled shader library (lsfg-vk.dll) contains the " +
                            "compiled SPIR-V shaders for the generate pass.",
                    link = "https://github.com/PancakeTAS/lsfg-vk  |  https://lsfg-vk.dev"
                )
                AttributionRow(
                    name = "Lossless Scaling (BSG)",
                    desc = "The original frame-generation algorithm and the Lossless.dll shader " +
                            "format that this project's dual-format loader supports.",
                    link = "https://www.blueskysoftware.net"
                )
            }
        }

        // ---- Secondary upstreams ----
        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                Text("Also derived from / informed by", style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.SemiBold)
                AttributionRow(
                    name = "scrcpy (Genymobile)",
                    desc = "The non-root capture path (MediaProjection -> VirtualDisplay -> ImageReader) " +
                            "and the general 'capture the screen, decode to a buffer, present elsewhere' " +
                            "architecture follow scrcpy's proven approach.",
                    link = "https://github.com/Genymobile/scrcpy  (MIT license)"
                )
                AttributionRow(
                    name = "volk (Vulkan Loader)",
                    desc = "The WSI presenter loads the Vulkan driver dynamically with volk (Anton " +
                            "Bikineev), avoiding a hard link to libvulkan.so.",
                    link = "https://github.com/zeux/volk"
                )
                AttributionRow(
                    name = "libsu / Superuser (topjohnwu)",
                    desc = "Root shell execution (screencap, su) is performed through the libsu 5.x API " +
                            "(com.topjohnwu.superuser), which manages root/shell providers.",
                    link = "https://github.com/topjohnwu/libsu  (MPL-2.0)"
                )
            }
        }

        // ---- Dependencies ----
        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                Text("Open-source dependencies", style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.SemiBold)
                Text(
                    "AndroidX (Compose, Activity, Lifecycle, Core), Kotlin, NDK/CMake toolchain, " +
                    "Vulkan-Headers (The Khronos Group), and the Android platform's AHardwareBuffer / " +
                    "MediaProjection APIs.",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
        }

        // ---- License / notice ----
        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("Notice", style = MaterialTheme.typography.titleSmall, fontWeight = FontWeight.SemiBold)
                Text(
                    "This is an independent, unofficial port. LSFG / Lossless Scaling and their marks " +
                    "belong to their respective owners. Use of a root/shell capture path may be subject " +
                    "to your device's warranty and the terms of your root solution (KernelSU / Magisk). " +
                    "Frame generation is a client-side modification and may violate the rules of some " +
                    "online games; use responsibly.",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    textAlign = TextAlign.Justify
                )
            }
        }

        Spacer(Modifier.height(24.dp))
    }
}

@Composable
private fun AttributionRow(name: String, desc: String, link: String) {
    Column(Modifier.fillMaxWidth().padding(vertical = 4.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
        Text(name, style = MaterialTheme.typography.titleSmall, fontWeight = FontWeight.SemiBold,
            color = Color(0xFF22D3EE))
        Text(desc, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurface)
        Text(link, style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}
