# FusionHUD — Technical Deep-Dive

This document describes how FusionHUD works, accurate to the shipped code in `:fusionhud`. It is meant
for developers integrating the library or syncing fixes from upstream Bannerlator.

- [Architecture: a composited overlay, not the Vulkan layer](#architecture)
- [The five sizes and their chips](#the-five-sizes)
- [FpsCounter: present-tick counting, the frametime ring, percentile lows](#fpscounter)
- [HudMetrics: the cached Snapshot and every metric's read path](#hudmetrics)
- [Cross-vendor probing](#cross-vendor-probing)
- [VRAM and the VK_EXT_memory_budget gap](#vram-and-the-vk_ext_memory_budget-gap)
- [Diagnostics export](#diagnostics-export)
- [Threading rules](#threading-rules)
- [Integration API](#integration-api)
- [The standalone shims](#the-standalone-shims)

## Architecture

FusionHUD is a single `android.view.View` subclass — `widget.fusionhud.FusionHudView` — that draws with
`Canvas` in `onDraw`. You add it to a container on top of your game surface. It is a **composited
overlay**, not a graphics-API layer.

**Why not the real MangoHud?** MangoHud is a Vulkan layer (`VK_LAYER_MANGOHUD_overlay`) that interposes on
`vkQueuePresentKHR` and renders its overlay *inside the application's own swapchain*. That requires the app
to load the layer (`VK_INSTANCE_LAYERS`/implicit layer JSON) and only works for Vulkan clients. FusionHUD
instead draws as an ordinary Android view above the game. Consequences:

- It is **renderer-agnostic** — GL, Vulkan, `zink`/wrapper, ANGLE all work, because nothing is hooked.
- There is **no layer plumbing** to inject or enable per driver.
- It reads its numbers from the **OS** (procfs/sysfs/`BatteryManager`), not from the graphics API. So a
  handful of GPU figures depend on what the kernel exposes for the device (see
  [Cross-vendor probing](#cross-vendor-probing)), and it cannot observe swapchain-internal timings the way
  an in-swapchain layer could. FPS is instead measured from **present ticks** you feed it (see
  [FpsCounter](#fpscounter)).

**Refresh cadence.** `FusionHudView` runs its own coroutine (`CoroutineScope(SupervisorJob() +
Dispatchers.Main.immediate)`), started in `onAttachedToWindow` and cancelled in `onDetachedFromWindow`.
Roughly once a second it does one `HudMetrics.snapshot()` off the draw path (see
[Threading](#threading-rules)), stores the immutable result, and invalidates. So the *only* thing on the
hot present path is `FpsCounter.tick()` — cheap counter math, no view access.

## The five sizes

There is **one** overlay style in five sizes, defined by `widget.fusionhud.FusionSize`
(`FULL`, `TILES`, `PILL`, `MINIMAL`, `MEGA`). The `token` is what is stored under the `hudSize` config
key; tapping the HUD calls `next()` (Full → Tiles → Pill → Minimal → Mega → Full).

`FusionSize.supportedChips()` is the **single source of truth** for which metric "chips" each size renders
— both this view and (in Bannerlator) the config UIs query it, so the chip list can't drift from what is
drawn. `buildConfig()` always emits every key regardless (hiding a chip is purely presentational).

| Size | Chips rendered |
|------|----------------|
| **Minimal** | `FPS`, `FPS graph`, `0.01% low`, `FPS .1`, `Engine`, `Clock` |
| **Pill** | `FPS`, `FPS graph`, `GPU`, `CPU`, `VRAM`, `RAM`, `Battery`, `GPU model`, `FPS .1`, `Engine`, `Clock` |
| **Full** / **Tiles** | `FPS`, `FPS graph`, `GPU`, `GPU temp`, `CPU`, `VRAM`, `RAM`, `Power`, `Temp`, `Battery`, `GPU model`, `Engine`, `0.01% low`, `FPS .1`, `Clock` |
| **Mega** | the Full set **plus** `Per-core`, `Swap`, `Network`, `Resolution`, `Proton`, `Wrapper`, `DX ver`, `Session` |

On Pill the battery temperature is drawn as a `BAT °C` chip after RAM, and a GPU name wider than the stats
gets its own line across the top. `Clock` (a subtle corner time readout) is in every set. The visual language (colours, accent, outline) is
shared across all sizes; only the amount of detail changes. The accent colour comes from
`ui.theme.AppThemeState.getCurrentAccentArgb()`.

## FpsCounter

`widget.FpsCounter` is the single authoritative FPS source. You call `tick()` **once per presented frame**
from your present thread; the view reads it. It is deliberately view-free so it is safe to call from the
render thread.

- **FPS math** — a 500 ms compute window: `tick()` counts frames; every ≥500 ms it recomputes
  `lastFPS = frames * 1000 / elapsed`. `getCurrentFPS()` is a lock-free volatile read.
- **Session stats** — sampled at a coarser 1 s cadence: `getAvgFPS()`, `getMinFPS()`, `getMaxFPS()`,
  `getSessionLengthSec()`. `writeSessionSummary(context)` persists a small JSON to the tmp dir on a
  background thread (uses the `ImageFs` shim for the path — see [shims](#the-standalone-shims)).
- **Frametime ring for percentile lows** — a bounded ring buffer (`FRAMETIME_RING_CAP = 8192`) records the
  inter-present interval (ms) each tick, skipping the first tick and implausibly long gaps
  (`> 10 s`, i.e. pause/alt-tab) so a stall doesn't poison it. `tick()` only appends (cheap). The lows are
  computed lazily on read: the buffer is copied under lock, sorted **outside** the lock (so `tick()` never
  blocks on a sort), and cached for 250 ms so the three getters share one sort.
  - `getLow1Pct()` — FPS at the 99th-percentile (worst 1%) frametime.
  - `getLow01Pct()` — worst 0.1%.
  - `getLow001Pct()` — worst 0.01% (needs ~10k frames to be meaningful; noisy before that, expected).
- **FPS decimal** — the `fpsDecimal` config key controls whether the drawn number shows a decimal place.
- `reset()` clears counters + the ring (call when the game window is unmapped).

## HudMetrics

`widget.HudMetrics` is the single, device-complete metric collector. No part of the view reads sysfs; it
all goes through here so the readers can't diverge.

The key method is **`snapshot()`**: one collection pass that reads *every* metric into an immutable
`HudMetrics.Snapshot` (all numeric fields nullable when unreadable). The view calls this on its ~1 Hz
coroutine and never touches sysfs/`BatteryManager` during `onDraw`. Per-device node discovery is done once
and cached in the `HudMetrics` instance.

Read paths, metric by metric:

- **CPU %** — `/proc/stat` aggregate delta since the last call (`(total-idle)/total`), with a fallback
  that estimates load from `scaling_cur_freq / cpuinfo_max_freq` across cores where `/proc/stat` is
  restricted. Per-core % likewise from `/proc/stat` per-cpu lines; per-core clock from each core's
  `cpufreq/scaling_cur_freq`. Peak CPU clock = max `scaling_cur_freq`.
- **GPU load** — see [cross-vendor probing](#cross-vendor-probing). Each candidate filename shape is parsed
  by `readGpuUsageSample`: Adreno `gpubusy` (busy/total pair), Mali `gpuinfo` (busy-ms delta over
  wall-clock delta, so it holds state between calls), or a plain percent node.
- **GPU clock** — a static list (Adreno KGSL `gpuclk`/`devfreq/cur_freq`, Mali vendor `gpu_clock`, Exynos
  `gpusysfs/gpu_clock`, generic devfreq `cur_freq`) plus a vendor-agnostic devfreq walk; Hz/kHz/MHz are
  normalized and sanity-clamped to 1..3000 MHz.
- **Temperature** — prioritized thermal-zone discovery over `/sys/class/thermal` +
  `/sys/devices/virtual/thermal`, separately ranked for CPU and GPU (see
  [cross-vendor probing](#cross-vendor-probing)); milli-°C values normalized and clamped to 1..150 °C to
  reject offline sensors. GPU temp also tries KGSL/Mali direct nodes first.
- **Temperature danger bands** — green/amber/red. In Auto mode the bands come from the **kernel's own
  declared trip points** for the zone actually read (`passive` → amber onset, `hot`/`critical` → red),
  filtered to plausible 20..120 °C so a battery-current-limiter "trip" of `1` can't be mistaken for a
  temperature; falls back to conservative constants (CPU/GPU 90 °C, battery 48 °C) when the device declares
  none. Manual mode uses the user's red point with amber a proportional step below.
- **RAM** — `ActivityManager.MemoryInfo` (`totalMem`, `availMem`) → used bytes + percent.
- **Swap** — `/proc/meminfo` (`SwapTotal`/`SwapFree`) → used/total bytes, or `null` when there is no swap.
- **Network** — `/proc/net/dev` summed across interfaces, differenced against the previous sample →
  down/up bytes-per-second (first sample returns `null`).
- **VRAM** — see [the VRAM section](#vram-and-the-vk_ext_memory_budget-gap).
- **Battery** — `collectBattery()` reads `BatteryManager` (`CAPACITY`, `CURRENT_NOW`, `CHARGE_COUNTER`)
  and the sticky `ACTION_BATTERY_CHANGED` intent (`EXTRA_VOLTAGE`, `EXTRA_TEMPERATURE`, `EXTRA_STATUS`).
  Power = |current| × voltage. **Sign quirk:** `current_now`'s sign convention is OEM-dependent (several
  OEMs report discharge as positive), so wattage uses the magnitude and the reliable charge *direction*
  comes from `EXTRA_PLUGGED`/`EXTRA_STATUS`. Only when the framework gives no usable charge state
  (`EXTRA_STATUS` unknown or no battery intent) does it check `/sys/class/power_supply/*`: a battery
  `status` of Charging/Full, or a charger supply with `online == 1`, means charging. Runtime-left = charge_counter / current, smoothed and clamped
  to ≤72 h; "LEFT CHG" while charging.

## Cross-vendor probing

The GPU-load, GPU-clock and thermal readers were broadened (this is the CI-green
`feat/hud-cross-vendor` hardening) so one build lights up across vendors. Discovery is cached per
`HudMetrics` instance.

**GPU load** — `discoverGpuUsagePaths()`:

1. An ordered static candidate list (`GPU_USAGE_STATIC_PATHS`), world-readable when present:
   - Adreno KGSL: `/sys/class/kgsl/kgsl-3d0/gpubusy`, `.../gpu_busy_percentage`, `.../devfreq/gpu_load`
   - Mali: `/sys/class/misc/mali0/device/{utilisation,utilization,gpuinfo}`,
     `/sys/devices/platform/mali/utilization`, `/sys/kernel/gpu/gpu_busy`, and the Exynos vendor node
     `/sys/devices/platform/gpusysfs/gpu_busy`
   - PowerVR (Rogue): `/sys/class/misc/pvrsrvkm/device/utilisation`, `/sys/class/pvr/utilisation`,
     `/sys/class/pvr/gpu_utilisation`
   - amdgpu / Samsung Xclipse ("sgpu"): `/sys/class/drm/card0/device/gpu_busy_percent`
   - generic devfreq: `/sys/class/devfreq/gpu/load`
   - MediaTek GED: `/sys/kernel/ged/hal/gpu_utilization` ("<loading> <block> <idle>"),
     `/sys/module/ged/parameters/gpu_loading` (plain percent)
2. A scan of `/sys/devices/platform/*` for nodes whose (hashed/addressed) directory name contains a GPU
   token — catches things like `/sys/devices/platform/13000000.mali/...`.
3. A walk of `/sys/class/devfreq` and `/sys/devices/virtual/devfreq` for any node whose path contains a
   GPU token, reading the percent-bearing files.

GPU tokens (`GPU_NODE_TOKENS`): `gpu`, `mali`, `g3d`, `kgsl`, `panfrost`, `pvr`, `powervr`, `xclipse`,
`sgpu`. Percent filenames scanned (`GPU_USAGE_FILES`): `gpu_busy_percentage`, `gpu_busy_percent`,
`gpu_load`, `utilisation`, `utilization`, `load`, `gpu_busy`, `gpuinfo`.

**GPU clock** — `GPU_CLOCK_PATHS` (Adreno KGSL, Mali vendor, Exynos `gpusysfs/gpu_clock`, MediaTek GED
`/sys/kernel/ged/hal/current_freqency` (the vendor's spelling, kHz) and `.../gpu_clock` (MHz), generic devfreq)
plus a vendor-agnostic devfreq walk for any `gpu/mali/panfrost/xclipse/sgpu` node's `cur_freq`.

**Thermal ranking** — zones are ranked so the most representative sensor wins:
- CPU: `cpu-silicon` > `cpu-0` > `cpu` (not gpu) > `s5p-tmu` (Exynos TMU) > `soc` > `cputop` > `tsens`
  (Qualcomm aggregate) > `cluster` > `big`/`little`.
- GPU: `gpu-silicon` > `gpuss` (Qualcomm) / `mtktsgpu` (MediaTek) > `gpu-virt` / `gpu` > `g3d` (Exynos
  Mali) > `kgsl` > `mali` > `xclipse`/`sgpu`.

Any device that still misses is exactly what the [diagnostics export](#diagnostics-export) is for.

## VRAM and the VK_EXT_memory_budget gap

Used VRAM is read in pure Java from the two vendors that expose a byte counter in sysfs:

- **Adreno KGSL** — `/sys/class/kgsl/kgsl-3d0/{gpumem_mapped,page_alloc,mapped,page_alloc_max}`
- **amdgpu / Xclipse (sgpu)** — `/sys/class/drm/card0/device/mem_info_vram_used` (and `_total`, used only
  in the diagnostics report)

Values are sanity-checked (≥1 MiB, ≤64 GiB) to reject idle-zero / non-byte nodes.

**Mali and PowerVR expose no VRAM byte counter in sysfs**, so on those devices VRAM reads `null` and the
row hides. The vendor-agnostic fallback is the Vulkan device extension **`VK_EXT_memory_budget`**, which is
a documented **`TODO(native)`**: it needs the native side to create an ephemeral `VkInstance`/
`VkPhysicalDevice` and query `VkPhysicalDeviceMemoryBudgetPropertiesEXT` — i.e. an **NDK rebuild**, out of
scope for a source-only library. (For reference, in Bannerlator the native `getRenderer` already spins an
ephemeral `VkInstance`; this library has no native blob, so `GPUInformation.getRenderer` is the EGL/inject
shim instead — see [shims](#the-standalone-shims).)

## Diagnostics export

`widget.HudDiagnosticsExport.exportHudDiagnostics(...)` is a one-tap "why is a row missing on my device?"
tool. It is manual and invoked-only — nothing here runs on the refresh path.

```kotlin
fun exportHudDiagnostics(
    context: Context,
    targetDir: File? = context.getExternalFilesDir(null),
    fileProviderAuthority: String? = null,
)
```

It runs `HudMetrics.buildDiagnosticsReport(context)` **on a background thread** (it primes the delta-based
readers, sleeps ~350 ms, then samples, so it must never touch the main thread), writes the plain-text
report to `targetDir`, runs the `MediaScanner` over it, and toasts the saved path. The `ACTION_SEND` share
sheet is built **only when `fileProviderAuthority != null`**.

- **Standalone default** — `targetDir = getExternalFilesDir(null)` (the app-specific external dir: writable
  with **no runtime permission**, works in the demo) and `fileProviderAuthority = null` (no share sheet).
- **Host-app sharing** — Bannerlator passes its public Downloads dir + `"${applicationId}.tileprovider"`;
  the demo passes the defaults.

The report contains:

- A header: device manufacturer/model, board/hardware, SoC (`Build.SOC_MANUFACTURER`/`SOC_MODEL` on
  Android 12+), Android version/API, CPU core count, GPU renderer and Vulkan version.
- A **per-metric** section: for each metric either `HIT` with the **resolved source path/API and current
  value**, or `MISS` with the **exact candidate paths it tried**.
- An **"Exists on this device"** dump (thermal zone types, devfreq/kgsl/mali/platform nodes present) so a
  device owner can reply with the real path for anything that missed — which is how a new device gets
  added to the candidate lists above.

## Threading rules

- **Draw is main-thread only.** `onDraw` reads the last `Snapshot` and the volatile FPS; it never touches
  sysfs.
- **`FpsCounter.tick()` is called from the present thread**, once per presented frame. It is synchronized
  counter math with no view access; `getCurrentFPS()` is a lock-free volatile read.
- **`HudMetrics.snapshot()` runs off the draw path** — on `FusionHudView`'s own coroutine (main dispatcher,
  but the work is a one-shot pass at ~1 Hz; if you call it yourself, keep it off the UI critical path).
- **`buildDiagnosticsReport()` must run on a background thread** (`exportHudDiagnostics` already does this).
- The frametime percentile sort in `FpsCounter` happens **outside** the tick lock, so `tick()` never blocks
  on it.

## Integration API

`FusionHudView` (constructors `(context)` and `(context, fpsProvider: () -> Float)`):

- `setFpsCounter(FpsCounter?)` — the shared FPS source; you `tick()` it per present.
- `applyConfig(String?)` — a `KeyValueSet` config string; keys include `hudSize`, `hudScale`, `hudLocked`,
  the per-chip `show*` flags, `fpsDecimal`, and the temperature options. `Container.DEFAULT_FPS_COUNTER_CONFIG`
  is a ready-made default.
- Labels: `setEngineLabel`, `setGpuModel`, `setWineVersion`, `setGraphicsWrapper` (and DXVK/VKD3D fields).
- Optional host-set readouts (off by default, any thread):
  - `setPresentedFps(Float)` — frames per second reaching the screen. `FpsCounter` counts the game's frames,
    so it can't see frames added or lost after the game (frame generation, a compositor dropping frames).
    When this differs from the game's rate by more than 10 % (and at least 1 fps) the FPS reads
    `game→shown`, e.g. `30→118` or `60→45`; `0` = off.
  - `setDisplayServer(String?)` — `"X11"` / `"Wayland"`, shown as a DISP row, tile or Pill suffix.
  - `setHdrState(Int)` — a `FusionHdr` code (`NONE`, `ON`, `NO_HEADROOM`, `OFF`, `READY`, `TONEMAPPED`,
    `NOT_ON_THIS_SCREEN`); `NONE` draws no HDR line.
  - `setGpuSpoofName(String?)` — a GPU name reported to games in place of the real one; the GPU row
    shows it, marked as a spoof.
- Listeners: `setOnSizeCycledListener(Consumer<String>)` (fires with the new size token on tap),
  `setOnMovedListener(BiConsumer<Float,Float>)` (drag), `setOnLockChangedListener(Consumer<Boolean>)`
  (long-press lock).

Supporting types: `FpsCounter`, `HudMetrics` (+ `Snapshot`, `TempDisplay`, `Battery`), `FusionSize`, `FusionHdr`,
`HudLockController`, and the diagnostics function.

`AppThemeState.setCurrentAccentArgb(Int)` sets the accent every view reads. `GPUInformation.setRenderer` /
`setVulkanVersion` inject the strings shown in labels/diagnostics when you have no native probe.

## The standalone shims

To keep the HUD source byte-identical to Bannerlator, the app-side dependencies are provided as small
same-package shims rather than by editing the HUD files:

- `core.KeyValueSet` — copied verbatim (the config-string parser).
- `core.StringUtils` — trimmed to just `replace(...)` (the only method `KeyValueSet` calls).
- `core.GPUInformation` — standalone: `extractModelName` verbatim; `isAdrenoGPU` substring check;
  `getRenderer` returns an injected string (`setRenderer`) or a cached throwaway-EGL `GL_RENDERER` probe;
  `getVulkanVersion` returns an injected string (`setVulkanVersion`) or `"Unknown"`. (Upstream these are
  native.)
- `container.Container` — only `DEFAULT_HUD_SCALE` and `DEFAULT_FPS_COUNTER_CONFIG` (values copied verbatim).
- `ui.theme.AppThemeState` — a settable accent (`get/setCurrentAccentArgb`, default `0xFFA374FF`).
- `xenvironment.ImageFs` — minimal stub: `find(context)` + `getTmpDir()` return a real writable app-private
  dir, which is all `FpsCounter.writeSessionSummary` needs.

The backend files kept byte-identical to upstream — `FpsCounter.java`, `HudMetrics.java`,
`HudLockController.java`, `fusionhud/FusionHudView.kt`, `fusionhud/FusionHudModels.kt` — carry only an added
GPL/§7(b) header on four of them (`FpsCounter`, `HudMetrics`, `HudLockController`, `FusionHudView`), so the
owner can sync fixes by copying the file body over. Some KDoc `[links]` in those comments still point at
now-removed classes (e.g. `PerformanceHudView`); an unresolved KDoc link is a doc artifact, not a compile
error, and is left intact on purpose.
