import {
  ButtonItem,
  DropdownItem,
  PanelSection,
  PanelSectionRow,
  SliderField,
  ToggleField,
  staticClasses
} from "@decky/ui";
import { callable, definePlugin } from "@decky/api";
import { useEffect, useState } from "react";
import { FaTachometerAlt } from "react-icons/fa";

type Hud = Record<string, string>;
type State = {
  enabled: boolean;
  overlay_running: boolean;
  reloaded: boolean | null;
  user_backup: boolean;
  hud_overlay: boolean;
  hud_running: boolean;
  hud_last_exit: number | null;
  hud_binary: boolean;
  hud: Hud;
};

const getState = callable<[], State>("get_state");
const setEnabled = callable<[enabled: boolean], State>("set_enabled");
const reapply = callable<[], State>("reapply");
const setOverlay = callable<[on: boolean], State>("set_overlay");
const setHud = callable<[key: string, value: string], State>("set_hud");
const resetHud = callable<[], State>("reset_hud");

// FusionSize.supportedChips() (FusionHudModels.kt), minus the switches a size never draws: FPS on Pill and
// Minimal (always shown there), graph and 0.01 % low on Tiles. Wrapper and DX version stay hidden: DroidDeck
// has no wrapper and nothing reports the DXVK/VKD3D version. Engine is gamescope's engine name ("DXVK" in
// Dead Cells), so it stays.
const SIZES = [  // FusionSize order, as Bannerlator's size picker
  { data: "full", label: "Full" },
  { data: "tiles", label: "Tiles" },
  { data: "pill", label: "Pill" },
  { data: "minimal", label: "Minimal" },
  { data: "mega", label: "Mega" }
];
const STANDARD = ["FPS", "FPS graph", "GPU model", "GPU", "GPU temp", "CPU", "RAM", "VRAM", "Temp", "Battery", "Power", "0.01% low", "FPS .1", "Engine", "Clock"];
const SIZE_CHIPS: Record<string, string[]> = {
  minimal: ["FPS graph", "0.01% low", "FPS .1", "Engine", "Clock"],
  // The Pill also draws battery temperature and watts, so their switches are listed here too.
  pill: ["GPU model", "GPU", "CPU", "RAM", "VRAM", "Temp", "Battery", "Power", "FPS .1", "Engine", "Clock"],
  full: STANDARD,
  tiles: STANDARD.filter((c) => c !== "FPS graph" && c !== "0.01% low"),
  mega: [...STANDARD, "Per-core", "Swap", "Network", "Resolution", "Proton", "Session"]
};

// Bannerlator's metric switches (DeckOverlaySettings.kt): label, config key, chip.
const METRICS: [string, string, string][] = [
  ["FPS", "showFPS", "FPS"],
  ["FPS graph", "showFPSGraph", "FPS graph"],
  ["GPU model", "showGpuModel", "GPU model"],
  ["GPU", "showGPULoad", "GPU"],
  ["GPU temperature", "showGpuTemp", "GPU temp"],
  ["CPU", "showCPUUsage", "CPU"],
  ["RAM", "showRAM", "RAM"],
  ["VRAM", "showVram", "VRAM"],
  ["Temperature", "showTemp", "Temp"],
  ["Battery", "showBattery", "Battery"],
  ["Power", "showPower", "Power"],
  ["0.01% low", "showLow001", "0.01% low"],
  ["FPS with a decimal", "fpsDecimal", "FPS .1"],
  ["Per-core CPU", "showPerCore", "Per-core"],
  ["Swap", "showSwap", "Swap"],
  ["Network", "showNet", "Network"],
  ["Resolution", "showResolution", "Resolution"],
  ["Proton", "showProton", "Proton"],
  ["Wrapper", "showWrapper", "Wrapper"],
  ["DirectX version", "showDxVer", "DX ver"],
  ["Session", "showSession", "Session"],
  ["Engine", "showEngine", "Engine"],
  ["Clock", "showClock", "Clock"]
];

const POSITIONS = [
  { data: "top-left", label: "Top left" },
  { data: "top-center", label: "Top centre" },
  { data: "top-right", label: "Top right" },
  { data: "bottom-left", label: "Bottom left" },
  { data: "bottom-right", label: "Bottom right" }
];
const COLOURS = [
  { data: "soft", label: "Soft" },
  { data: "mid", label: "Mid" },
  { data: "vivid", label: "Vivid" }
];
const ACCENTS = [
  { data: "A374FF", label: "Purple" },
  { data: "00E5FF", label: "Cyan" },
  { data: "FF5252", label: "Red" },
  { data: "69F0AE", label: "Green" },
  { data: "FFAB5E", label: "Orange" },
  { data: "F2F5F9", label: "White" }
];

const small = { fontSize: "12px", lineHeight: "16px", opacity: 0.8 };

function status(s: State): string {
  if (!s.hud_binary) return "The overlay program is missing from this plugin build.";
  if (!s.hud_overlay) return "Off.";
  if (s.hud_running) return "Running. It shows while Steam's Performance Overlay Level is OFF.";
  if (s.hud_last_exit !== null) return `Stopped (exit ${s.hud_last_exit}); retrying.`;
  return "Starting…";
}

function Content() {
  const [s, setS] = useState<State | null>(null);
  useEffect(() => {
    getState().then(setS);
    const timer = setInterval(() => getState().then(setS), 3000);
    return () => clearInterval(timer);
  }, []);
  if (!s) return <PanelSection><PanelSectionRow>Loading…</PanelSectionRow></PanelSection>;

  const hud = s.hud;
  const set = async (key: string, value: string) => setS(await setHud(key, value));
  const on = (key: string) => hud[key] === "1";
  const toggle = (label: string, key: string, description?: string) => (
    <PanelSectionRow key={key}>
      <ToggleField label={label} description={description} checked={on(key)}
        onChange={(v: boolean) => set(key, v ? "1" : "0")} />
    </PanelSectionRow>
  );
  const slider = (label: string, key: string, min: number, max: number, step: number, suffix: string) => (
    <PanelSectionRow key={key}>
      <SliderField label={label} value={parseInt(hud[key]) || min} min={min} max={max} step={step}
        showValue valueSuffix={suffix} onChange={(v: number) => set(key, String(v))} />
    </PanelSectionRow>
  );
  const dropdown = (label: string, key: string, options: { data: string; label: string }[]) => (
    <PanelSectionRow key={key}>
      <DropdownItem label={label} rgOptions={options} selectedOption={hud[key]}
        onChange={(o: { data: string }) => set(key, o.data)} />
    </PanelSectionRow>
  );

  const size = SIZE_CHIPS[hud.hudSize] ? hud.hudSize : "pill";
  const chips = SIZE_CHIPS[size];
  const tempsShown = (chips.includes("Temp") && on("showTemp")) || (chips.includes("GPU temp") && on("showGpuTemp"));
  const bands = hud.tempBands === "0" ? "0" : hud.tempAuto === "0" ? "2" : "1";

  return (
    <>
      <PanelSection title="FusionHUD overlay">
        <PanelSectionRow>
          <ToggleField label="FusionHUD" checked={s.hud_overlay}
            onChange={async (v: boolean) => setS(await setOverlay(v))} />
        </PanelSectionRow>
        <PanelSectionRow><div style={small}>{status(s)}</div></PanelSectionRow>
      </PanelSection>

      <PanelSection title="Overlay">
        {dropdown("Size", "hudSize", SIZES)}
        {dropdown("Position", "hudPosition", POSITIONS)}
      </PanelSection>

      <PanelSection title="Look">
        {slider("Size on screen", "hudScale", 50, 150, 5, "%")}
        {slider("Opacity", "hudOpacity", 0, 100, 5, "%")}
        {dropdown("Colour", "hudColor", COLOURS)}
        {slider("Outline", "hudOutline", 0, 100, 10, "")}
        {dropdown("Outline colour", "hudOutlineAccent", [{ data: "0", label: "Gray" }, { data: "1", label: "Accent" }])}
        {dropdown("Accent", "hudAccent", ACCENTS)}
      </PanelSection>

      <PanelSection title="Metrics">
        {METRICS.filter(([, , chip]) => chips.includes(chip)).map(([label, key]) => toggle(label, key))}
      </PanelSection>

      {tempsShown && (
        <PanelSection title="Temperatures">
          {dropdown("Unit", "tempUnit", [{ data: "c", label: "°C" }, { data: "f", label: "°F" }])}
          <PanelSectionRow>
            <DropdownItem label="Danger colours" selectedOption={bands}
              rgOptions={[{ data: "0", label: "Off" }, { data: "1", label: "Auto" }, { data: "2", label: "Manual" }]}
              onChange={async (o: { data: string }) => {
                await setHud("tempBands", o.data === "0" ? "0" : "1");
                setS(await setHud("tempAuto", o.data === "2" ? "0" : "1"));
              }} />
          </PanelSectionRow>
          {bands === "2" && slider("CPU red at", "tempRedCpu", 50, 110, 1, "°C")}
          {bands === "2" && slider("GPU red at", "tempRedGpu", 50, 110, 1, "°C")}
          {bands === "2" && slider("Battery red at", "tempRedBat", 35, 60, 1, "°C")}
        </PanelSection>
      )}

      <PanelSection>
        <PanelSectionRow>
          <ButtonItem layout="below" onClick={async () => setS(await resetHud())}>Reset overlay settings</ButtonItem>
        </PanelSectionRow>
      </PanelSection>

      <PanelSection title="Steam overlay styles">
        <PanelSectionRow>
          <ToggleField label="FusionHUD colours for Steam's overlay" checked={s.enabled}
            description="Optional: restyles Steam's Performance Overlay levels 1–4 (Minimal, Pill, Full, Mega)."
            onChange={async (v: boolean) => setS(await setEnabled(v))} />
        </PanelSectionRow>
        {s.user_backup && (
          <PanelSectionRow><div style={small}>Your earlier presets.conf is saved and comes back when this is off.</div></PanelSectionRow>
        )}
        {s.enabled && (
          <PanelSectionRow>
            <ButtonItem layout="below" onClick={async () => setS(await reapply())}>Apply again</ButtonItem>
          </PanelSectionRow>
        )}
      </PanelSection>
    </>
  );
}

export default definePlugin(() => ({
  name: "FusionHUD",
  titleView: <div className={staticClasses.Title}>FusionHUD</div>,
  content: <Content />,
  icon: <FaTachometerAlt />,
  onDismount() {}
}));
