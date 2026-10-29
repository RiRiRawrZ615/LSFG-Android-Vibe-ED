package com.lsfg.fork.ui

import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color

private val DarkScheme = darkColorScheme(
    primary = Color(0xFF22D3EE),
    onPrimary = Color(0xFF001418),
    primaryContainer = Color(0xFF0E7490),
    onPrimaryContainer = Color(0xFFCFFAFE),
    secondary = Color(0xFF67E8F9),
    onSecondary = Color(0xFF00222B),
    background = Color(0xFF0B0F14),
    onBackground = Color(0xFFE2E8F0),
    surface = Color(0xFF111826),
    onSurface = Color(0xFFE2E8F0),
    surfaceVariant = Color(0xFF1E293B),
    onSurfaceVariant = Color(0xFF94A3B8),
    outline = Color(0xFF334155),
    error = Color(0xFFF87171),
)

private val LightScheme = lightColorScheme(
    primary = Color(0xFF0891B2),
    onPrimary = Color(0xFFFFFFFF),
    background = Color(0xFFF8FAFC),
    onBackground = Color(0xFF0F172A),
    surface = Color(0xFFFFFFFF),
    onSurface = Color(0xFF0F172A),
)

@Composable
fun LSFGTheme(darkTheme: Boolean = isSystemInDarkTheme(), content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme = if (darkTheme) DarkScheme else LightScheme,
        content = content
    )
}
