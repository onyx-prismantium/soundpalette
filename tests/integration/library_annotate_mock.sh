#!/usr/bin/env bash
# Extension-4 §11: the annotator protocol against the mock annotator (mcp/dist/annotate_mock.js)
# in every failure mode. No model, no network. Requires the MCP package built (mcp_smoke.sh or
# `npm run build` in mcp/).
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT_DIR"

BIN="./build/cli/soundpalette"
[[ -f "./build/cli/soundpalette.exe" ]] && BIN="./build/cli/soundpalette.exe"
MOCK="mcp/dist/annotate_mock.js"
FX="tests/golden/fixtures"
if [[ ! -x "$BIN" || ! -d "$FX" ]]; then
    echo "library_annotate_mock.sh: missing binary or fixtures (build + genfixtures first)" >&2
    exit 2
fi
if ! command -v node >/dev/null; then
    echo "library_annotate_mock.sh: node not found" >&2
    exit 2
fi
if [[ ! -f "$MOCK" ]]; then
    (cd mcp && { [[ -d node_modules ]] || npm ci; } && npm run build) >/dev/null
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
LIB="$WORK/lib"
mkdir -p "$LIB/ui" "$LIB/combat"
cp "$FX/click.wav" "$LIB/ui/UIClick_Soft Tap_SP_TEST.wav"
cp "$FX/click.wav" "$LIB/ui/menu_click_confirm.wav"
cp "$FX/bright_outlier.wav" "$LIB/combat/gun_pistol_shot_01.wav"
cp "$FX/sine440_1s.wav" "$LIB/tone.wav"
cat > "$WORK/answers.json" <<'EOF'
{
  "tone.wav": {"description": "A steady pure sine tone holds at one pitch.", "fx_name": "Sine Tone Steady",
               "category": "DESIGNED", "keywords": ["sine", "tone", "steady", "pure", "synth"],
               "confidence": 0.9, "choose": "DSGNTonl", "choose_confidence": 0.85},
  "gun_pistol_shot_01.wav": {"description": "A single dry pistol shot with a short room tail.",
               "fx_name": "Pistol Shot Dry", "category": "GUNS",
               "keywords": ["pistol", "shot", "gun", "dry", "single"], "confidence": 0.8, "choose": "GUNPis"}
}
EOF
export SP_MOCK_ANSWERS="$WORK/answers.json"
ANN="node $MOCK"

"$BIN" library init "$LIB" --quiet > /dev/null

field() { python3 -c "import json,sys; j=json.load(open('$1')); print(j$2)"; }

# 1. normal: only the unannotated row (tone.wav) is sent; the model's pick lands with provenance.
"$BIN" library annotate "$LIB" --annotator "$ANN" --json > "$WORK/n.json"
[[ "$(field "$WORK/n.json" "['requested']")" == "1" ]] || { echo "FAIL: normal requested" >&2; exit 1; }
[[ "$(field "$WORK/n.json" "['annotated']")" == "1" ]] || { echo "FAIL: normal annotated" >&2; exit 1; }
"$BIN" library show "$LIB/tone.wav" --json > "$WORK/tone.json"
[[ "$(field "$WORK/tone.json" "['annotation']['cat_id']")" == "DSGNTonl" ]] || { echo "FAIL: cat_id" >&2; exit 1; }
[[ "$(field "$WORK/tone.json" "['annotation']['source']")" == "model" ]] || { echo "FAIL: source" >&2; exit 1; }
[[ "$(field "$WORK/tone.json" "['annotation']['model']")" == "mock-normal" ]] || { echo "FAIL: model id" >&2; exit 1; }
[[ "$(field "$WORK/tone.json" "['annotation']['prompt_version']")" == "mock_v1" ]] || { echo "FAIL: prompt version" >&2; exit 1; }
"$BIN" library search "$LIB" sine steady | grep -q "tone.wav" || { echo "FAIL: FTS over model description" >&2; exit 1; }

# 2. precedence: filename-sourced rows are upgraded by the model (--all), human rows never.
"$BIN" library set "$LIB/ui/menu_click_confirm.wav" --catid UIClick --description "Human said so." > /dev/null
"$BIN" library annotate "$LIB" --all --annotator "$ANN" --json > "$WORK/a.json"
[[ "$(field "$WORK/a.json" "['skipped_locked']")" == "1" ]] || { echo "FAIL: locked row not skipped" >&2; exit 1; }
[[ "$(field "$WORK/a.json" "['annotated']")" == "3" ]] || { echo "FAIL: --all annotated count" >&2; exit 1; }
"$BIN" library show "$LIB/ui/menu_click_confirm.wav" | grep -q "Human said so." || { echo "FAIL: human edit lost" >&2; exit 1; }
"$BIN" library show "$LIB/combat/gun_pistol_shot_01.wav" | grep -q "source      model" || { echo "FAIL: model did not upgrade filename row" >&2; exit 1; }

# 3. dry run writes nothing.
"$BIN" library set "$LIB/ui/menu_click_confirm.wav" --unlock > /dev/null
BEFORE="$("$BIN" library show "$LIB/tone.wav" --json)"
SP_MOCK_MODE=outside_shortlist "$BIN" library annotate "$LIB" --all --dry-run --annotator "$ANN" --json > "$WORK/d.json"
[[ "$("$BIN" library show "$LIB/tone.wav" --json)" == "$BEFORE" ]] || { echo "FAIL: dry run wrote" >&2; exit 1; }

# 4. failure matrix (each mode is one script run; --force so every row is sent).
run_mode() { SP_MOCK_MODE="$1" "$BIN" library annotate "$LIB" --all --force --annotator "$ANN" --timeout 3 --inflight 2 --json 2>/dev/null > "$WORK/$1.json" || true; }

run_mode malformed
[[ "$(field "$WORK/malformed.json" "['errors']")" == "0" ]] || { echo "FAIL: malformed line should be ignored" >&2; exit 1; }

run_mode unknown_category
[[ "$(field "$WORK/unknown_category.json" "['errors']")" == "0" ]] || { echo "FAIL: unknown category should degrade, not error" >&2; exit 1; }
"$BIN" library show "$LIB/tone.wav" --json | grep -q '"cat_id": "DSGNTonl"' || { echo "FAIL: unknown category should still resolve via category-less shortlist" >&2; exit 1; }

run_mode outside_shortlist
python3 - "$WORK/outside_shortlist.json" <<'PY'
import json, sys
j = json.load(open(sys.argv[1]))
assert j["errors"] == 0, j
for f in j["files"]:
    assert "shortlist top" in f["status"], f
    assert f["annotation"]["confidence"] <= 0.5, f
print("outside_shortlist ok")
PY

run_mode error
[[ "$(field "$WORK/error.json" "['errors']")" == "4" ]] || { echo "FAIL: error mode" >&2; exit 1; }
[[ "$(field "$WORK/error.json" "['fatal']")" == "" ]] || { echo "FAIL: error mode must not be fatal" >&2; exit 1; }

run_mode timeout
[[ "$(field "$WORK/timeout.json" "['timeouts']")" == "1" ]] || { echo "FAIL: timeout count" >&2; exit 1; }
[[ "$(field "$WORK/timeout.json" "['restarts']")" -ge 1 ]] || { echo "FAIL: timeout must restart" >&2; exit 1; }
[[ "$(field "$WORK/timeout.json" "['annotated']")" == "3" ]] || { echo "FAIL: run must continue after a timeout" >&2; exit 1; }

run_mode crash
[[ "$(field "$WORK/crash.json" "['annotated']")" == "0" ]] || { echo "FAIL: crash mode annotated" >&2; exit 1; }
[[ "$(field "$WORK/crash.json" "['restarts']")" -le 6 ]] || { echo "FAIL: restart cap" >&2; exit 1; }

run_mode no_choose
[[ "$(field "$WORK/no_choose.json" "['errors']")" == "0" ]] || { echo "FAIL: no_choose" >&2; exit 1; }
grep -q "no choose stage" "$WORK/no_choose.json" || { echo "FAIL: no_choose note" >&2; exit 1; }

# 5. unreachable command: fatal, non-zero exit, nothing written.
if "$BIN" library annotate "$LIB" --all --annotator "sp-no-such-annotator-zzz" --json > "$WORK/x.json" 2>/dev/null; then
    echo "FAIL: unreachable annotator should exit non-zero" >&2
    exit 1
fi
[[ -n "$(field "$WORK/x.json" "['fatal']")" ]] || { echo "FAIL: fatal missing" >&2; exit 1; }

# 6. --min-confidence re-annotates low rows only (human row untouched even though unlocked).
SP_MOCK_MODE=normal "$BIN" library annotate "$LIB" --all --force --annotator "$ANN" --json > /dev/null
"$BIN" library annotate "$LIB" --min-confidence 0.9 --annotator "$ANN" --json > "$WORK/m.json"
[[ "$(field "$WORK/m.json" "['requested']")" -ge 1 ]] || { echo "FAIL: min-confidence selection" >&2; exit 1; }

echo "library_annotate_mock.sh: PASS"
