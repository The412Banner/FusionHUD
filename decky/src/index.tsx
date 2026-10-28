import {
  ButtonItem,
  PanelSection,
  PanelSectionRow,
  ToggleField,
  staticClasses
} from "@decky/ui";
import { callable, definePlugin, toaster } from "@decky/api";
import { useEffect, useState } from "react";
import { FaTachometerAlt } from "react-icons/fa";

type State = {
  enabled: boolean;
  presets_path: string;
  presets_active: boolean;
  user_backup: boolean;
  overlay_config: string;
  overlay_running: boolean;
  reloaded: boolean | null;
};

const getState = callable<[], State>("get_state");
const setEnabled = callable<[enabled: boolean], State>("set_enabled");
const reapply = callable<[], State>("reapply");

const LEVELS = ["1  Minimal", "2  Pill", "3  Full", "4  Mega"];

function note(state: State): string {
  if (!state.overlay_running) return "Steam's performance overlay isn't running. Turn on Performance overlay in DroidDeck's Steam settings.";
  if (state.reloaded === false) return "Couldn't reload the overlay; the style applies when you change the level.";
  return state.enabled ? "Pick a level in Steam's quick menu (Performance) to see each style." : "Steam's own overlay styles are back.";
}

function Content() {
  const [state, setState] = useState<State | null>(null);

  useEffect(() => {
    getState().then(setState);
  }, []);

  if (!state) return <PanelSection><PanelSectionRow>Loading…</PanelSectionRow></PanelSection>;

  return (
    <PanelSection title="Performance overlay">
      <PanelSectionRow>
        <ToggleField
          label="FusionHUD style"
          description="Restyles Steam's performance overlay levels 1–4."
          checked={state.enabled}
          onChange={async (on: boolean) => {
            const next = await setEnabled(on);
            setState(next);
            toaster.toast({ title: "FusionHUD", body: on ? "FusionHUD style on" : "Steam's style back" });
          }}
        />
      </PanelSectionRow>
      <PanelSectionRow>
        <div style={{ fontSize: "12px", lineHeight: "18px", opacity: 0.8 }}>
          {LEVELS.map((l) => <div key={l}>Level {l}</div>)}
        </div>
      </PanelSectionRow>
      <PanelSectionRow>
        <div style={{ fontSize: "12px", lineHeight: "16px", opacity: 0.8 }}>{note(state)}</div>
      </PanelSectionRow>
      {state.user_backup && (
        <PanelSectionRow>
          <div style={{ fontSize: "12px", lineHeight: "16px", opacity: 0.8 }}>
            Your earlier presets.conf is saved and comes back when you turn this off.
          </div>
        </PanelSectionRow>
      )}
      <PanelSectionRow>
        <ButtonItem layout="below" disabled={!state.enabled} onClick={async () => setState(await reapply())}>
          Apply again
        </ButtonItem>
      </PanelSectionRow>
    </PanelSection>
  );
}

export default definePlugin(() => ({
  name: "FusionHUD",
  titleView: <div className={staticClasses.Title}>FusionHUD</div>,
  content: <Content />,
  icon: <FaTachometerAlt />,
  onDismount() {}
}));
