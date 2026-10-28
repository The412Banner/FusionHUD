# FusionHUD for Decky (DroidDeck)

A Decky Loader plugin that gives Steam's performance overlay the [FusionHUD](https://github.com/The412Banner/FusionHUD) look. It writes MangoHud presets, so Steam's overlay levels become:

| Steam level | FusionHUD size |
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
- Writes `~/.config/MangoHud/presets.conf` (`$XDG_CONFIG_HOME/MangoHud/presets.conf`), which Steam's overlay (mangoapp) reads for each level.
- To apply a change at once it rewrites Steam's overlay config (`$MANGOHUD_CONFIGFILE`, `/tmp/droiddeck-mangohud.conf` in DroidDeck) with the same contents, which makes the overlay reload.
- A `presets.conf` you already had is saved first and put back when you turn the switch off or uninstall the plugin.

## Build
```
npm install
npm run build
```
Package the folder as one top folder `FusionHUD/` with `plugin.json`, `package.json`, `dist/index.js`, `main.py`, `presets/`, `LICENSE`, `README.md`.

## License
GPL-3.0, like FusionHUD; credit The412Banner (https://github.com/The412Banner/FusionHUD). Project layout from the [Decky plugin template](https://github.com/SteamDeckHomebrew/decky-plugin-template) (BSD-3-Clause).
