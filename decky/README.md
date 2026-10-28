# FusionHUD for Decky (DroidDeck)

A Decky Loader plugin that brings [FusionHUD](https://github.com/The412Banner/FusionHUD) to DroidDeck's Steam session.

- **FusionHUD overlay:** the real FusionHUD drawn over games by its own small program (`bin/fusionhud-overlay`), switched on and set up from the plugin's panel: size, position, size on screen, opacity, colour, outline, accent, the metric switches and temperature colours, with the same settings as Bannerlator's HUD. Steam's own performance overlay stays off: gamescope shows one overlay at a time, and FusionHUD's shows while Steam's Performance Overlay Level is OFF. Sizes so far: Pill and Minimal.
- **Steam overlay styles (optional):** FusionHUD colours for Steam's own performance overlay levels 1-4, through MangoHud presets.

| Steam level | Style with "Steam overlay styles" on |
|---|---|
| 1 | Minimal: FPS, frame-time graph, 1 % / 0.1 % lows, clock |
| 2 | Pill: a small rounded box with FPS, GPU, CPU, RAM, VRAM, battery, clock (Steam's overlay always draws its one-row layout across the full screen width, so the Pill is a compact box instead) |
| 3 | Full: GPU, CPU, RAM, VRAM, battery, FPS, average and 1 % / 0.1 % lows, graph, clock |
| 4 | Mega: Full plus per-core CPU, swap, network, resolution, refresh rate, display server, Wine, present mode, session time |

## Install (DroidDeck)
1. DroidDeck → Steam settings → Decky: install Decky and turn it on.
2. Turn on **Performance overlay** in the same screen.
3. **Plugins → Install from ZIP** and pick the plugin ZIP (no Steam session may be running).
4. Start Steam, open the quick menu → Performance, and pick a level.

## How it works
- **Overlay:** `fusionhud-overlay` opens a transparent OpenGL window on gamescope's X display and marks it `GAMESCOPE_EXTERNAL_OVERLAY` (no message type), as MangoHud's mangoapp does. gamescope then sends it the game's frame times on the untagged mangoapp message queue (type 1), separate from the stream Steam's own overlay reads. CPU, GPU, RAM, battery and temperatures come from `/proc` and `/sys`, as in FusionHUD's `HudMetrics`. It re-reads its settings file (`overlay.conf` in the plugin's settings folder) when the panel changes it. The overlay gets no touch input, so size and position are set in the panel.
- **Steam overlay styles:** writes `~/.config/MangoHud/presets.conf` (`$XDG_CONFIG_HOME/MangoHud/presets.conf`), which Steam's overlay reads for each level, and rewrites `$MANGOHUD_CONFIGFILE` (`/tmp/droiddeck-mangohud.conf`) with the same contents so it reloads. A `presets.conf` you already had is saved first and put back when you turn this off or uninstall the plugin.

## Build
GitHub Actions (`.github/workflows/decky.yml`) builds the overlay on an arm64 runner (CMake, GLFW 3.4 and Dear ImGui v1.91.9b fetched at pinned versions), builds the panel (`npm ci && npm run build`) and packages `FusionHUD.zip` with one top folder `FusionHUD/`: `plugin.json`, `package.json`, `main.py`, `dist/index.js`, `bin/fusionhud-overlay`, `presets/`, `LICENSE`, `README.md`.

## License
GPL-3.0, like FusionHUD; credit The412Banner (https://github.com/The412Banner/FusionHUD). Project layout from the [Decky plugin template](https://github.com/SteamDeckHomebrew/decky-plugin-template) (BSD-3-Clause). The overlay window and frame-time queue follow [MangoHud](https://github.com/flightlessmango/MangoHud)'s mangoapp (MIT); it uses [Dear ImGui](https://github.com/ocornut/imgui) (MIT) and [GLFW](https://www.glfw.org) (zlib).
