# FusionHUD

A lightweight, drop-in **in-game performance HUD** for Android — extracted from
[Bannerlator](https://github.com/The412Banner) so other Winlator-fork developers can add a polished perf
overlay without re-implementing the sysfs/procfs plumbing.

FusionHUD is a **composited overlay**: a single Android `View` (`FusionHudView`) that draws with
`Canvas` on top of your game surface, refreshing itself about once a second **off** the render/present
path. It is **not** the real MangoHud Vulkan layer. MangoHud is a Vulkan layer (`VK_LAYER_MANGOHUD_overlay`)
that interposes on `vkQueuePresentKHR` and draws inside the app's own swapchain; FusionHUD's flagship look
is *MangoHud-inspired* but is drawn by the host app as an ordinary overlay view. That trade is deliberate:

- **Works on any renderer** — GL, Vulkan, `zink`/wrapper, ANGLE — because it never hooks the driver.
- **No layer plumbing** — nothing to inject, no `VK_INSTANCE_LAYERS`, no per-driver enablement.
- The cost: it reads metrics from the OS (procfs/sysfs/`BatteryManager`), not from the graphics API, so a
  few GPU figures depend on what the kernel exposes (see the cross-vendor probing below), and it can't see
  swapchain-internal timings the way an in-swapchain layer can.

Built as classic Android Views (`Canvas` drawing) — **no Jetpack Compose**. Self-refreshing on a
coroutine (`kotlinx-coroutines-android`); you just tick FPS once per presented frame.

> Deep technical dive (architecture, every metric's read path, the diagnostics format, threading, API):
> **[docs/FUSION_HUD.md](docs/FUSION_HUD.md)**.

## One style, five sizes

FusionHUD ships a **single** overlay — the MangoHud-inspired **Fusion** style (`widget.fusionhud.FusionHudView`)
— in **five sizes**. Tap the HUD to cycle sizes, or set `hudSize=<token>` in the config string.
(Enum: `FusionSize`; the chip set each size renders is the single source of truth in
`FusionSize.supportedChips()`.)

| Size | Token | What it shows |
|------|-------|---------------|
| **Full** | `full` | Standard chip set: FPS (+graph, 0.01% low), GPU / GPU-temp, CPU, VRAM, RAM, Power, Temp, Battery, GPU model, Engine, Clock. |
| **Tiles** | `tiles` | Same standard chip set, laid out as tiles. |
| **Pill** | `pill` | Compact pill: FPS (+graph), GPU, CPU, VRAM, Battery, GPU model, Clock. |
| **Minimal** | `minimal` | Just FPS (+graph, 0.01% low, decimal) and a subtle Clock. |
| **Mega** | `mega` | Everything in Full **plus** per-core CPU, Swap, Network, Resolution, Proton, Wrapper, DX version, Session length. |

A subtle corner clock and the tap-to-cycle / drag-to-move / long-press-to-lock gestures are available in
every size.

## Features

- FPS (optional decimal), session avg/min/max, and **percentile lows** (1% / 0.1% / 0.01%) from a
  per-present frametime ring buffer.
- CPU / GPU load, CPU / GPU clock, CPU / GPU temperature with green/amber/red **danger bands** driven by
  the device's own kernel thermal trip points when it declares them.
- RAM, VRAM, swap, per-core CPU load + clock, network up/down (Mega).
- Battery %, power (W), temperature, and a smoothed runtime-left estimate.
- Engine / GPU-model / Wine(Proton) / wrapper / DXVK / VKD3D labels you feed from your emulator.
- Configurable accent colour, background opacity, outline, scale, and clock.
- **Cross-vendor metric probing** (below) so the same build lights up on Adreno, Mali, PowerVR, Xclipse/
  amdgpu, panfrost, MediaTek and Exynos parts.
- **One-tap diagnostics export** — writes a plain-text report of exactly which sysfs path each metric
  resolved to (or MISS + the candidates it tried + an "exists on this device" dump), so device owners can
  send back the right node for anything that missed. See `exportHudDiagnostics(...)`.

## How metrics are read

All readings come from `widget.HudMetrics`, a single hardened collector (no part of the view touches
sysfs itself). `HudMetrics.snapshot()` reads **everything once per refresh** into an immutable `Snapshot`,
so `onDraw` never touches sysfs/`BatteryManager`. Discovery of the right node per device is done once and
cached. Highlights (full detail in [docs/FUSION_HUD.md](docs/FUSION_HUD.md)):

- **CPU** — `/proc/stat` aggregate delta, with a `scaling_cur_freq / cpuinfo_max_freq` fallback where
  `/proc/stat` is restricted; per-core % and clock from `cpufreq`.
- **GPU load / clock — cross-vendor** — an ordered static candidate list (Adreno KGSL `gpubusy`; Mali
  `misc/mali0` + Exynos `gpusysfs`; PowerVR `pvr`; amdgpu/Xclipse `drm/card0/.../gpu_busy_percent`) **plus**
  a dynamic scan of `/sys/devices/platform/*` and `/sys/class/devfreq` for any node whose name contains a
  GPU token (`gpu`, `mali`, `g3d`, `kgsl`, `panfrost`, `pvr`, `powervr`, `xclipse`, `sgpu`). Each filename
  shape (Adreno `gpubusy` busy/total, Mali `gpuinfo` delta-over-wall-clock, plain percent) is parsed
  correctly. Discovery cached.
- **Temperature** — prioritized CPU and GPU thermal-zone discovery (incl. Qualcomm `gpuss`/`tsens`,
  MediaTek `mtktsgpu`, Exynos `g3d`/`s5p-tmu`, Xclipse/`sgpu`), milli-°C normalization + a 1..150 °C sanity
  clamp. Danger bands prefer the kernel's declared `passive`/`critical` trip points.
- **VRAM** — Adreno KGSL (bytes mapped/allocated) and amdgpu-style `drm/card0` (`mem_info_vram_used`) in
  pure Java. Mali/PowerVR expose no byte counter in sysfs → they read `null` today and the row hides.
  A vendor-agnostic `VK_EXT_memory_budget` fallback is a documented **TODO(native)** (needs an NDK rebuild).
- **Battery** — `BatteryManager` for %, current, charge-counter, temperature; power is the current×voltage
  magnitude (the `current_now` sign convention is OEM-dependent, so charge *direction* comes from
  `EXTRA_PLUGGED`).

Reads are all standard app-sandbox reads — **no runtime permission and no root required**. Nodes a device
doesn't expose simply return `null` and that row hides itself.

## Install

FusionHUD ships as a Gradle **`:fusionhud` library module** (an `.aar`). Add the module to your build
(or publish the AAR), then depend on it:

```gradle
dependencies {
    implementation project(':fusionhud')
    // transitively pulls kotlinx-coroutines-android + androidx.appcompat/core-ktx
}
```

> **Namespace note:** the library module's `namespace` is `com.winlator.star` (not `.hud`) purely for
> package consistency — every shipped file declares a `com.winlator.star.*` package, so matching the
> namespace keeps the HUD **byte-identical to upstream Bannerlator** and lets the owner sync fixes with a
> plain file copy. (There are no resources in this module anymore, so there's no `R`-resolution constraint.)

Requires: `compileSdk 34`, `minSdk 26`, AGP 8.8.0, Kotlin 2.0.21, Java 17. Build everything standalone with:

```
./gradlew :fusionhud:assembleRelease :demo:assembleDebug
```

## Integration guide

```kotlin
// 1. Add the overlay on top of your game surface (a plain View; add it to your overlay container).
val hud = FusionHudView(context)
gameOverlayContainer.addView(hud)

// 2. Give it the single authoritative FPS source and tick it once per PRESENTED frame.
val fpsCounter = FpsCounter()
hud.setFpsCounter(fpsCounter)
// From your render/present thread (e.g. the X-server epoll thread, or a Choreographer callback):
fpsCounter.tick()            // counter math only, thread-safe, no view access

// 3. Configure it with a KeyValueSet config string (what to show, size, scale, lock, temp units…).
hud.applyConfig(Container.DEFAULT_FPS_COUNTER_CONFIG)   // or your own "hudSize=pill,showGPU=1,..."

// 4. Feed the descriptive labels your emulator already knows.
hud.setEngineLabel("DXVK 2.4")
hud.setGpuModel("Adreno 750")
hud.setWineVersion("Proton 10.0-4")
hud.setGraphicsWrapper("GameNative")

// 5. Set your brand accent colour (packed ARGB). The HUD reads this.
AppThemeState.setCurrentAccentArgb(0xFF00E5FF.toInt())

// 6. Wire the interaction listeners (tap cycles size; drag moves; long-press toggles lock).
hud.setOnSizeCycledListener { token -> saveHudSize(token) }
hud.setOnMovedListener { x, y -> saveHudPosition(x, y) }
hud.setOnLockChangedListener { locked -> saveHudLocked(locked) }
```

`FusionHudView` **self-refreshes** — a coroutine on the main dispatcher, started in `onAttachedToWindow`
and cancelled in `onDetachedFromWindow`, calls `HudMetrics.snapshot()` about once a second. You do **not**
poll it; you only tick the `FpsCounter` per presented frame. If you have no `FpsCounter`, `FusionHudView`
also accepts a `(context, fpsProvider: () -> Float)` constructor.

If you don't run a native GPU probe, call `GPUInformation.setRenderer("<renderer>")` (and optionally
`GPUInformation.setVulkanVersion("<ver>")`) once so the labels/diagnostics show real values; otherwise
FusionHUD falls back to a throwaway-EGL `GL_RENDERER` probe.

### Diagnostics export

```kotlin
// Standalone default: writes to the app-specific external dir (no runtime permission), no share sheet.
exportHudDiagnostics(context)

// With sharing (host app supplies a FileProvider): writes to your dir + opens an ACTION_SEND chooser.
exportHudDiagnostics(context, myDownloadsDir, "${applicationId}.tileprovider")
```

`exportHudDiagnostics(context, targetDir = context.getExternalFilesDir(null), fileProviderAuthority = null)`
runs `HudMetrics.buildDiagnosticsReport` off the main thread, writes the report, runs the MediaScanner and
toasts the path; the share sheet is only built when `fileProviderAuthority != null`.

See the **`:demo`** module (`DemoActivity`) for a complete, runnable example — a fake animated "game" with
the HUD overlaid, FPS ticked via `Choreographer`, and buttons to cycle size / accent / lock and export
diagnostics.

## Credits / Required Attribution

FusionHUD is © **The412Banner** and licensed under **GPL-3.0** with an **additional GPL-3.0 §7(b)
attribution requirement**. Any use, fork, or distribution **must credit "The412Banner"
(https://github.com/The412Banner) in your project documentation AND in your app's in-app credits/about
screen**, with a link to **https://github.com/The412Banner/FusionHUD**.

**Please read [`ATTRIBUTION.md`](ATTRIBUTION.md) — the attribution is mandatory, not optional.** The
per-file copyright headers must also be preserved.
