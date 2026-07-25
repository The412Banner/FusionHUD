package com.winlator.star.core;

import java.util.Locale;

/**
 * Standalone trim of Bannerlator's {@code StringUtils}, containing only the members the HUD
 * subsystem (and the {@link KeyValueSet} it ships with) actually reference:
 * <ul>
 *   <li>{@link #replace(String, int, int, String)} — used by {@link KeyValueSet#put}.</li>
 *   <li>{@link #formatBytes(long)} / {@link #formatBytes(long, boolean)} — used by the classic
 *       {@code FrameRating} / {@code FrameRatingHorizontal} overlays for the RAM row.</li>
 * </ul>
 * The bodies are copied verbatim from the upstream so behaviour is identical. If you sync a HUD fix
 * that starts calling another {@code StringUtils} method, add it here (same signature).
 */
public class StringUtils {

    public static String replace(String text, int start, int end, String value) {
        return text.substring(0, start) + value + text.substring(end);
    }

    public static String formatBytes(long bytes) {
        return formatBytes(bytes, true);
    }

    public static String formatBytes(long bytes, boolean withSuffix) {
        if (bytes <= 0) return "0 bytes";
        final String[] units = new String[]{"bytes", "KB", "MB", "GB", "TB"};
        int digitGroups = (int)(Math.log10(bytes) / Math.log10(1024));
        String suffix = withSuffix ? " "+units[digitGroups] : "";
        return String.format(Locale.ENGLISH, "%.2f", bytes / Math.pow(1024, digitGroups))+suffix;
    }
}
