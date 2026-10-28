import asyncio
import json
import os
import shutil
import subprocess

import decky

# First line of the presets file this plugin writes; anything else at that path belongs to the user.
MARK = "FusionHUD Decky plugin presets"


def _config_dir() -> str:
    # DroidDeck exports XDG_CONFIG_HOME=$HOME/.config before gamescope and Decky start, and mangoapp
    # reads <config dir>/MangoHud/presets.conf (MangoHud overlay_params.cpp parse_preset_config).
    return os.environ.get("XDG_CONFIG_HOME") or os.path.join(decky.DECKY_USER_HOME, ".config")


PRESETS = os.path.join(_config_dir(), "MangoHud", "presets.conf")
SOURCE = os.path.join(decky.DECKY_PLUGIN_DIR, "presets", "fusion-presets.conf")
SETTINGS = os.path.join(decky.DECKY_PLUGIN_SETTINGS_DIR, "settings.json")
BACKUP = os.path.join(decky.DECKY_PLUGIN_SETTINGS_DIR, "presets.conf.before-fusionhud")
# Steam writes the overlay level here; mangoapp reloads (and re-reads presets.conf) when it is modified.
OVERLAY_CONFIG = os.environ.get("MANGOHUD_CONFIGFILE", "/tmp/droiddeck-mangohud.conf")

OVERLAY_BIN = os.path.join(decky.DECKY_PLUGIN_DIR, "bin", "fusionhud-overlay")
HUD_CONF = os.path.join(decky.DECKY_PLUGIN_SETTINGS_DIR, "overlay.conf")
OVERLAY_LOG = os.path.join(decky.DECKY_PLUGIN_LOG_DIR, "overlay.log")

# FusionHUD's own defaults (FusionHudView.applyConfig) plus the overlay's position and accent.
HUD_DEFAULTS = {
    "hudSize": "pill", "hudPosition": "top-left", "hudScale": "100", "hudOpacity": "80",
    "hudColor": "vivid", "hudOutline": "40", "hudOutlineAccent": "1", "hudAccent": "A374FF",
    "showFPS": "1", "showFPSGraph": "0", "showCPUUsage": "1", "showCPULoad": "1", "showGPULoad": "1",
    "showVram": "1", "showRAM": "1", "showPower": "1", "showTemp": "1", "showBatteryTemp": "0",
    "showGpuTemp": "0", "showBattery": "1", "showLow001": "1", "fpsDecimal": "1", "showPerCore": "1",
    "showSwap": "1", "showNet": "1", "showResolution": "1", "showProton": "1", "showWrapper": "1",
    "showDxVer": "1", "showSession": "1", "showClock": "1", "showEngine": "1", "showRenderer": "1",
    "showGpuModel": "1", "tempUnit": "c", "tempBands": "1", "tempAuto": "1",
    "tempRedCpu": "90", "tempRedGpu": "90", "tempRedBat": "48",
}
# Bannerlator sets these pairs together (DeckOverlaySettings "twins").
HUD_TWINS = {"showCPUUsage": ["showCPULoad"], "showTemp": ["showBatteryTemp"], "showEngine": ["showRenderer"]}


def _ours(path: str) -> bool:
    try:
        with open(path, encoding="utf-8") as f:
            return f.readline().startswith(MARK)
    except OSError:
        return False


def _load_settings() -> dict:
    try:
        with open(SETTINGS, encoding="utf-8") as f:
            s = json.load(f)
    except (OSError, ValueError):
        s = {}
    s.setdefault("enabled", False)   # Steam overlay presets
    s.setdefault("overlay", True)    # the FusionHUD overlay
    hud = dict(HUD_DEFAULTS)
    hud.update({k: str(v) for k, v in s.get("hud", {}).items()})
    s["hud"] = hud
    return s


def _save_settings(settings: dict) -> None:
    os.makedirs(os.path.dirname(SETTINGS), exist_ok=True)
    tmp = SETTINGS + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(settings, f, indent=1)
    os.replace(tmp, SETTINGS)


def _write_hud_conf(hud: dict) -> None:
    os.makedirs(os.path.dirname(HUD_CONF), exist_ok=True)
    tmp = HUD_CONF + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        f.write("".join(f"{k}={v}\n" for k, v in sorted(hud.items())))
    os.replace(tmp, HUD_CONF)


# ---- Steam overlay presets (MangoHud) ------------------------------------------------------------

def _install() -> None:
    os.makedirs(os.path.dirname(PRESETS), exist_ok=True)
    if os.path.isfile(PRESETS) and not _ours(PRESETS) and not os.path.exists(BACKUP):
        os.makedirs(os.path.dirname(BACKUP), exist_ok=True)
        shutil.copy2(PRESETS, BACKUP)
        decky.logger.info(f"kept the existing {PRESETS} as {BACKUP}")
    tmp = PRESETS + ".fusionhud-tmp"
    shutil.copyfile(SOURCE, tmp)
    os.replace(tmp, PRESETS)
    decky.logger.info(f"wrote {PRESETS}")


def _remove() -> None:
    if os.path.isfile(PRESETS) and _ours(PRESETS):
        os.remove(PRESETS)
        decky.logger.info(f"removed {PRESETS}")
    if os.path.isfile(BACKUP) and not os.path.exists(PRESETS):
        os.replace(BACKUP, PRESETS)
        decky.logger.info(f"put back {PRESETS} from {BACKUP}")


def _reload_overlay() -> bool:
    # Rewrite Steam's overlay config with the same bytes in place: the IN_MODIFY makes mangoapp reload.
    # Replacing the file instead would leave mangoapp watching the old one.
    try:
        with open(OVERLAY_CONFIG, "r+b") as f:
            data = f.read()
            f.seek(0)
            f.write(data)
            f.truncate()
        return True
    except OSError as e:
        decky.logger.info(f"overlay config {OVERLAY_CONFIG} not reloaded: {e}")
        return False


# ---- FusionHUD overlay process --------------------------------------------------------------------

class Overlay:
    def __init__(self):
        self.proc = None
        self.last_exit = None
        self.restarts = 0

    def running(self) -> bool:
        return self.proc is not None and self.proc.poll() is None

    def start(self) -> None:
        if self.running():
            return
        if not os.path.isfile(OVERLAY_BIN):
            decky.logger.error(f"{OVERLAY_BIN} is missing")
            return
        os.chmod(OVERLAY_BIN, 0o755)
        # Decky's bundled Python sets LD_LIBRARY_PATH to its own libraries; the overlay uses the system's.
        env = dict(os.environ)
        if "LD_LIBRARY_PATH_ORIG" in env:
            env["LD_LIBRARY_PATH"] = env["LD_LIBRARY_PATH_ORIG"]
        else:
            env.pop("LD_LIBRARY_PATH", None)
        os.makedirs(os.path.dirname(OVERLAY_LOG), exist_ok=True)
        log = open(OVERLAY_LOG, "ab")
        self.proc = subprocess.Popen([OVERLAY_BIN, HUD_CONF], env=env, cwd="/", stdout=log, stderr=log,
                                     stdin=subprocess.DEVNULL)
        log.close()
        decky.logger.info(f"overlay started, pid {self.proc.pid}, DISPLAY={env.get('DISPLAY', '')}")

    def stop(self) -> None:
        if self.proc is None:
            return
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        decky.logger.info(f"overlay stopped ({self.proc.returncode})")
        self.proc = None


OVERLAY = Overlay()


def _state(reloaded=None) -> dict:
    s = _load_settings()
    return {
        "enabled": s["enabled"],
        "presets_path": PRESETS,
        "presets_active": _ours(PRESETS),
        "user_backup": os.path.isfile(BACKUP),
        "overlay_config": OVERLAY_CONFIG,
        "overlay_running": os.path.isfile(OVERLAY_CONFIG),
        "reloaded": reloaded,
        "hud_overlay": s["overlay"],
        "hud_running": OVERLAY.running(),
        "hud_last_exit": OVERLAY.last_exit,
        "hud_binary": os.path.isfile(OVERLAY_BIN),
        "hud": s["hud"],
    }


class Plugin:
    async def get_state(self) -> dict:
        return _state()

    # Steam overlay presets
    async def set_enabled(self, enabled: bool) -> dict:
        settings = _load_settings()
        settings["enabled"] = bool(enabled)
        _save_settings(settings)
        if enabled:
            _install()
        else:
            _remove()
        return _state(_reload_overlay())

    async def reapply(self) -> dict:
        if _load_settings()["enabled"]:
            _install()
        return _state(_reload_overlay())

    # FusionHUD overlay
    async def set_overlay(self, on: bool) -> dict:
        settings = _load_settings()
        settings["overlay"] = bool(on)
        _save_settings(settings)
        OVERLAY.restarts = 0
        if on:
            _write_hud_conf(settings["hud"])
            OVERLAY.start()
        else:
            OVERLAY.stop()
        return _state()

    async def set_hud(self, key: str, value: str) -> dict:
        if key not in HUD_DEFAULTS:
            decky.logger.error(f"unknown HUD setting {key}")
            return _state()
        settings = _load_settings()
        settings["hud"][key] = str(value)
        for twin in HUD_TWINS.get(key, []):
            settings["hud"][twin] = str(value)
        _save_settings(settings)
        _write_hud_conf(settings["hud"])  # the overlay re-reads it when it changes
        return _state()

    async def reset_hud(self) -> dict:
        settings = _load_settings()
        settings["hud"] = dict(HUD_DEFAULTS)
        _save_settings(settings)
        _write_hud_conf(settings["hud"])
        return _state()

    async def _watch(self):
        # Brings the overlay back if it exits (a new session's display, a crash), with a growing pause.
        while True:
            await asyncio.sleep(5)
            if not _load_settings()["overlay"] or OVERLAY.running():
                continue
            if OVERLAY.proc is not None:
                OVERLAY.last_exit = OVERLAY.proc.returncode
                decky.logger.info(f"overlay exited with {OVERLAY.last_exit}")
                OVERLAY.proc = None
            if OVERLAY.restarts >= 5:
                continue
            await asyncio.sleep(min(30, 3 * OVERLAY.restarts))
            OVERLAY.restarts += 1
            OVERLAY.start()

    async def _main(self):
        settings = _load_settings()
        if settings["enabled"]:
            _install()
            _reload_overlay()
        _write_hud_conf(settings["hud"])
        if settings["overlay"]:
            OVERLAY.start()
        asyncio.get_event_loop().create_task(self._watch())

    async def _unload(self):
        OVERLAY.stop()

    async def _uninstall(self):
        OVERLAY.stop()
        _remove()
        _reload_overlay()
