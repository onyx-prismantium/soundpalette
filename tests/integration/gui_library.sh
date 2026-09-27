#!/usr/bin/env bash
# Extension-4 §11 GUI pass for the Library tab: runs the real app on a virtual X server, drives
# it with xdotool (open the Library tab, click a row, type a description into the inspector's
# annotation editor, save), verifies through the CLI that the human edit landed and is locked,
# and leaves screenshots in $OUT (default /tmp/sp-gui-library) for a visual check.
# Needs: Xvfb, xdotool, import (ImageMagick). Skipped (exit 0 with a notice) when any is
# missing, so the plain integration loop stays green on machines without a GUI toolchain.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT_DIR"
APP="./build/app/soundpalette-app"
BIN="./build/cli/soundpalette"
FX="tests/golden/fixtures"
OUT="${OUT:-/tmp/sp-gui-library}"
for t in Xvfb xdotool import; do
    command -v "$t" >/dev/null || { echo "gui_library.sh: $t missing, skipping" >&2; exit 0; }
done
[[ -x "$APP" && -x "$BIN" && -d "$FX" ]] || { echo "gui_library.sh: build + fixtures first" >&2; exit 2; }

WORK="$(mktemp -d)"
DISPLAY_NO=":$((90 + RANDOM % 20))"
cleanup() {
    [[ -n "${APP_PID:-}" ]] && kill "$APP_PID" 2>/dev/null || true
    [[ -n "${XVFB_PID:-}" ]] && kill "$XVFB_PID" 2>/dev/null || true
    rm -rf "$WORK"
}
trap cleanup EXIT
mkdir -p "$OUT" "$WORK/lib/ui" "$WORK/lib/combat"
cp "$FX/click.wav" "$WORK/lib/ui/UIClick_Soft Tap_SP_TEST.wav"
cp "$FX/click.wav" "$WORK/lib/ui/menu_click_confirm.wav"
cp "$FX/bright_outlier.wav" "$WORK/lib/combat/gun_pistol_shot_01.wav"
cp "$FX/sine440_1s.wav" "$WORK/lib/tone.wav"
"$BIN" library init "$WORK/lib" --quiet > /dev/null

Xvfb "$DISPLAY_NO" -screen 0 1280x800x24 > /dev/null 2>&1 &
XVFB_PID=$!
sleep 1
export DISPLAY="$DISPLAY_NO"
SP_UI_SCALE=1 "$APP" --dir "$WORK/lib" --view library --annotator "sp-no-such-annotator" > "$WORK/app.log" 2>&1 &
APP_PID=$!
sleep 3
kill -0 "$APP_PID" || { echo "gui_library.sh: app exited early"; cat "$WORK/app.log"; exit 1; }

shot() { import -display "$DISPLAY_NO" -window root "$OUT/$1.png"; }
# press and release in separate frames: ImGui needs to see the button down before the up
click() { xdotool mousemove --sync "$1" "$2"; sleep 0.15; xdotool mousedown 1; sleep 0.15; xdotool mouseup 1; sleep 0.5; }

# first click only focuses the freshly mapped window
click 640 500
shot 01_library_tab
# The table: first row's path cell (layout from the smoke capture: tree 190 px, header at y~90).
click 470 108
shot 02_row_selected
# Inspector annotation editor: the description field sits below CatID + FX name inputs.
# Click into the description input (right panel), type, then Save.
click 1090 170
xdotool key ctrl+a
xdotool type --delay 20 "Hand written in the GUI."
sleep 0.3
shot 03_typed_description
# Save button is the first button under the keyword field.
click 985 215
sleep 0.5
shot 04_saved
# Close the app (the text field may hold keyboard focus, so fall back to a signal).
xdotool key ctrl+q
sleep 1
kill "$APP_PID" 2>/dev/null || true
wait "$APP_PID" 2>/dev/null || true

# Verify through the CLI that a human annotation exists somewhere and is locked. Which row was
# hit depends on the window layout, so accept any row.
"$BIN" library search "$WORK/lib" --limit 0 --json > "$WORK/rows.json"
python3 - "$WORK/rows.json" <<'PY'
import json, sys
rows = json.load(open(sys.argv[1]))["results"]
human = [r for r in rows if r["annotation"] and r["annotation"]["source"] == "human"]
if not human:
    print("gui_library.sh: FAIL — no human annotation written (check the screenshots)")
    sys.exit(1)
r = human[0]
assert r["annotation"]["locked"], r
assert "Hand written" in r["annotation"]["description"], r
print("gui_library.sh: human edit on", r["path"], "locked =", r["annotation"]["locked"])
PY
echo "gui_library.sh: PASS (screenshots in $OUT)"
