# FusionHUD

A lightweight, drop-in **in-game performance HUD** for Android — extracted from
[Bannerlator](https://github.com/The412Banner) so other Winlator-fork developers can add a polished perf
overlay without re-implementing the sysfs/procfs plumbing.

FusionHUD is a **composited overlay** — plain Android `Canvas`/`View` overlays you place on top of your
game surface. It is **not** the real MangoHud Vulkan layer; the flagship **Fusion** style is
*MangoHud-inspired* but drawn by the app, so it works on any renderer (GL, Vulkan, wrapper/zink, …)
without hooking the driver.

- Pure classic Views (`Canvas` drawing) — **no Jetpack Compose**.
- Self-refreshing on a coroutine (`kotlinx-coroutines-android`); you just tick FPS per frame.
- One shared, device-complete metric collector — no overlay re-reads sysfs, so readings never diverge.

## Overlay styles

| Style | Class | Notes |
|-------|-------|-------|
| **Fusion** (MangoHud-inspired) | `widget.fusionhud.FusionHudView` | The flagship. 5 sizes (below). Self-refreshing. |
| **Performance** (GameNative-style) | `widget.perfhud.PerformanceHudView` | Stacked/compact metric tiles + graphs. Self-refreshing. |
| **PerfHud** (GameHub-style) | `widget.PerfHudView` | Compact Canvas HUD. Call `update()` ~1 Hz. |
| **Classic vertical** | `widget.FrameRating` | XML-inflated rows. Call `update()` ~1 Hz. |
| **Classic horizontal** | `widget.FrameRatingHorizontal` | XML-inflated pill. Call `update()` ~1 Hz. |

### Fusion sizes

`full` · `tiles` · `pill` · `minimal` · `mega` — same visual language, five amounts of detail.
Tap the HUD to cycle sizes, or set `hudSize=<token>` in the config string. (Enum: `FusionSize`.)

## Features

- FPS (with optional decimal), session avg/min/max, and **percentile lows** (1% / 0.1% / 0.01%) from a
  per-present frametime ring buffer.
- CPU / GPU load, CPU / GPU clock, CPU / GPU temperature (with green/amber/red danger bands driven by the
  device's own thermal trip points when available).
- RAM, VRAM (Adreno KGSL), swap, per-core CPU, network up/down (Mega).
- Battery %, power (W), temperature, and a smoothed runtime-left estimate.
- Engine / GPU-model / Wine(Proton) / wrapper / DXVK / VKD3D labels.
- Configurable accent colour, background opacity, outline, scale, and a subtle corner clock.

## How metrics are read

All readings come from `widget.HudMetrics`, a single hardened collector (no overlay touches sysfs itself):

- **CPU** — `/proc/stat` aggregate delta, with a `scaling_cur_freq / cpuinfo_max_freq` fallback where
  `/proc/stat` is restricted.
- **GPU load** — ~10 static candidates plus a dynamic `/sys/class/devfreq` + `/sys/devices/virtual/devfreq`
  walk for gpu/mali/g3d/kgsl nodes; handles Adreno `gpubusy`, Mali `gpuinfo` (delta-over-wall-clock), and
  generic percent nodes. Discovery is cached.
- **Temperature** — prioritized CPU and GPU thermal-zone discovery (cpu-silicon > cpu-0 > … ; kgsl/mali
  direct paths for GPU), milli-°C normalization + a 1..150 °C sanity clamp. Danger bands prefer the
  kernel's declared `passive`/`critical` trip points.
- **Battery** — `BatteryManager` for %, current, charge-counter and temperature; power is the
  current×voltage magnitude (sign convention is OEM-dependent, so direction comes from `EXTRA_PLUGGED`).
- **Cached snapshot** — `HudMetrics.snapshot()` reads **everything once per refresh** so the view never
  touches sysfs/`BatteryManager` during `onDraw`. Cross-vendor probing is discovered once and cached.

Reads are all standard app-sandbox reads (procfs/sysfs/`BatteryManager`) — **no runtime permission and no
root required**. Nodes a given device doesn't expose simply return `null` and that row hides itself.

## Install

FusionHUD ships as a Gradle **`:fusionhud` library module** (an `.aar`). Add the module to your build
(or publish the AAR), then depend on it:

```gradle
dependencies {
    implementation project(':fusionhud')
    // transitively pulls kotlinx-coroutines-android + androidx.appcompat/core-ktx
}
```

> **Namespace note:** the library module's `namespace` is `com.winlator.star` (not `.hud`) on purpose —
> the classic overlays `import com.winlator.star.R`, and AGP generates `R` at the module namespace. Keep
> it as-is so the HUD source stays byte-identical to upstream Bannerlator and fixes sync by simple copy.

Requires: `compileSdk 34`, `minSdk 26`, AGP 8.8.0, Kotlin 2.0.21, Java 17. Build all standalone with:

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

// 5. Set your brand accent colour (packed ARGB). Every overlay reads this.
AppThemeState.setCurrentAccentArgb(0xFF00E5FF.toInt())

// 6. Wire the interaction listeners (tap cycles size; drag moves; long-press toggles lock).
hud.setOnSizeCycledListener { token -> saveHudSize(token) }
hud.setOnMovedListener { x, y -> saveHudPosition(x, y) }
hud.setOnLockChangedListener { locked -> saveHudLocked(locked) }
```

`FusionHudView` and `PerformanceHudView` **self-refresh** (a coroutine on the main dispatcher, started in
`onAttachedToWindow`). The classic overlays (`FrameRating`, `FrameRatingHorizontal`, `PerfHudView`) are
**not** self-refreshing — call their `update()` about once a second. All five expose `setFpsCounter`,
`applyConfig(String)`, and the tap/move/lock listeners. If you have no `FpsCounter`, `FusionHudView` /
`PerformanceHudView` also accept a `(context, fpsProvider: () -> Float)` constructor.

If you don't run a native GPU probe, call `GPUInformation.setRenderer("<your renderer string>")` once so
the classic overlays show a GPU name; otherwise FusionHUD falls back to a throwaway-EGL `GL_RENDERER`
probe. See the **`:demo`** module (`DemoActivity`) for a complete, runnable example — a fake animated
"game" with the HUD overlaid, FPS ticked via `Choreographer`, and buttons to switch style/size/accent/lock.

## Credits / Required Attribution

FusionHUD is © **The412Banner** and licensed under **GPL-3.0** with an **additional GPL-3.0 §7(b)
attribution requirement**. Any use, fork, or distribution **must credit "The412Banner"
(https://github.com/The412Banner) in your project documentation AND in your app's in-app credits/about
screen**, with a link to **https://github.com/The412Banner/FusionHUD**.

**Please read [`ATTRIBUTION.md`](ATTRIBUTION.md) — the attribution is mandatory, not optional.** The
per-file copyright headers must also be preserved.
