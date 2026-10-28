import json
import os
import shutil

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


def _ours(path: str) -> bool:
    try:
        with open(path, encoding="utf-8") as f:
            return f.readline().startswith(MARK)
    except OSError:
        return False


def _load_settings() -> dict:
    try:
        with open(SETTINGS, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return {"enabled": True}


def _save_settings(settings: dict) -> None:
    os.makedirs(os.path.dirname(SETTINGS), exist_ok=True)
    tmp = SETTINGS + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(settings, f)
    os.replace(tmp, SETTINGS)


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


def _state(reloaded=None) -> dict:
    return {
        "enabled": _load_settings().get("enabled", True),
        "presets_path": PRESETS,
        "presets_active": _ours(PRESETS),
        "user_backup": os.path.isfile(BACKUP),
        "overlay_config": OVERLAY_CONFIG,
        "overlay_running": os.path.isfile(OVERLAY_CONFIG),
        "reloaded": reloaded,
    }


class Plugin:
    async def get_state(self) -> dict:
        return _state()

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
        if _load_settings().get("enabled", True):
            _install()
        return _state(_reload_overlay())

    async def _main(self):
        if _load_settings().get("enabled", True):
            _install()
            _reload_overlay()

    async def _unload(self):
        pass

    async def _uninstall(self):
        _remove()
        _reload_overlay()
