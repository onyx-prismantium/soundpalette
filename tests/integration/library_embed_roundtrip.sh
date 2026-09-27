#!/usr/bin/env bash
# Extension-4 §11 (M18): write-back round trip. embed is a dry run by default; --apply rewrites
# only iXML/bext (data chunk hash unchanged, other chunks preserved), the fields read back on a
# fresh ingest as source "metadata", FLAC/OGG are refused, rename composes UCS names, refuses
# collisions and keeps annotations attached to the moved rows.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT_DIR"
BIN="./build/cli/soundpalette"
FX="tests/golden/fixtures"
[[ -x "$BIN" && -d "$FX" ]] || { echo "library_embed_roundtrip.sh: build + fixtures first" >&2; exit 2; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
LIB="$WORK/lib"
mkdir -p "$LIB/ui" "$LIB/amb"
cp "$FX/click.wav" "$LIB/ui/menu_click_confirm.wav"
cp "$FX/bright_outlier.wav" "$LIB/amb/forest_night_loop.wav"
cp "$FX/sine440_1s.flac" "$LIB/amb/wind_howl_desert.flac"
cp "$FX/decay_t60.wav" "$LIB/untagged_thing.wav"
"$BIN" library init "$LIB" --quiet > /dev/null
"$BIN" library set "$LIB/ui/menu_click_confirm.wav" --description "A soft confirm tap." --keywords "ui,click,confirm" > /dev/null
"$BIN" library set "$LIB/amb/wind_howl_desert.flac" --catid WINDTonl --fx-name "Desert Wind Howl" > /dev/null
"$BIN" library set "$LIB/amb/forest_night_loop.wav" --catid AMBForst --fx-name "Forest Night Loop" --description "Night forest bed with crickets." > /dev/null

field() { python3 -c "import json,sys; j=json.load(open('$1')); print(j$2)"; }

# 1. dry run: reports, writes nothing
sha_before="$(sha256sum "$LIB/ui/menu_click_confirm.wav" | cut -d' ' -f1)"
"$BIN" library embed "$LIB" --json > "$WORK/dry.json"
[[ "$(field "$WORK/dry.json" "['applied']")" == "False" ]] || { echo "FAIL: dry run flagged applied" >&2; exit 1; }
[[ "$(field "$WORK/dry.json" "['written']")" == "2" ]] || { echo "FAIL: dry run count ($(cat "$WORK/dry.json"))" >&2; exit 1; }
[[ "$(field "$WORK/dry.json" "['skipped']")" == "1" ]] || { echo "FAIL: FLAC should be skipped" >&2; exit 1; }
[[ "$(sha256sum "$LIB/ui/menu_click_confirm.wav" | cut -d' ' -f1)" == "$sha_before" ]] || { echo "FAIL: dry run modified a file" >&2; exit 1; }

# 2. apply with backup: data hash preserved, backup identical to the original, fields readable
"$BIN" library embed "$LIB" --apply --backup --json > "$WORK/apply.json"
[[ "$(field "$WORK/apply.json" "['written']")" == "2" ]] || { echo "FAIL: apply count" >&2; exit 1; }
[[ "$(field "$WORK/apply.json" "['errors']")" == "0" ]] || { echo "FAIL: apply errors" >&2; exit 1; }
[[ -f "$LIB/ui/menu_click_confirm.wav.bak" ]] || { echo "FAIL: no backup" >&2; exit 1; }
cmp -s "$LIB/ui/menu_click_confirm.wav.bak" "$FX/click.wav" || { echo "FAIL: backup differs from original" >&2; exit 1; }
[[ "$(sha256sum "$LIB/ui/menu_click_confirm.wav" | cut -d' ' -f1)" != "$sha_before" ]] || { echo "FAIL: apply did not write" >&2; exit 1; }
grep -a -q "<CATID>UIClick</CATID>" "$LIB/ui/menu_click_confirm.wav" || { echo "FAIL: iXML CatID missing" >&2; exit 1; }
grep -a -q "Menu Click Confirm - A soft confirm tap." "$LIB/ui/menu_click_confirm.wav" || { echo "FAIL: bext description missing" >&2; exit 1; }
rm -f "$LIB"/*/*.bak

# 3. the rewritten file still analyzes identically (only metadata changed): re-scan analysis
#    equals the stored entry; a fresh library reads the fields back as source "metadata".
"$BIN" library update "$LIB" --json > "$WORK/u.json"
[[ "$(field "$WORK/u.json" "['update']['changed']")" == "2" ]] || { echo "FAIL: rewritten files not seen as changed" >&2; exit 1; }
"$BIN" library show "$LIB/ui/menu_click_confirm.wav" --json > "$WORK/show.json"
[[ "$(field "$WORK/show.json" "['annotation']['source']")" == "human" ]] || { echo "FAIL: human annotation lost on update" >&2; exit 1; }
FRESH="$WORK/fresh"
mkdir -p "$FRESH"
cp -r "$LIB/ui" "$LIB/amb" "$FRESH/"
"$BIN" library init "$FRESH" --quiet > /dev/null
"$BIN" library classify "$FRESH" --from-metadata --json > "$WORK/meta.json"
[[ "$(field "$WORK/meta.json" "['written']")" == "2" ]] || { echo "FAIL: metadata classify count ($(cat "$WORK/meta.json"))" >&2; exit 1; }
"$BIN" library show "$FRESH/ui/menu_click_confirm.wav" --json > "$WORK/fresh_show.json"
[[ "$(field "$WORK/fresh_show.json" "['annotation']['source']")" == "metadata" ]] || { echo "FAIL: metadata source" >&2; exit 1; }
[[ "$(field "$WORK/fresh_show.json" "['annotation']['description']")" == "A soft confirm tap." ]] || { echo "FAIL: description from iXML" >&2; exit 1; }
# metadata outranks a name-based guess but not a human edit
"$BIN" library classify "$FRESH" --json > "$WORK/name_pass.json"
[[ "$(field "$WORK/name_pass.json" "['skipped_precedence']")" -ge 1 ]] || { echo "FAIL: name pass should not overwrite metadata ($(cat "$WORK/name_pass.json"))" >&2; exit 1; }
[[ "$(field "$WORK/fresh_show.json" "['annotation']['source']")" == "metadata" ]] || { echo "FAIL: metadata overwritten" >&2; exit 1; }

# 4. rename: dry run then apply; already-UCS names skipped; collision refused; rows follow.
"$BIN" library rename "$LIB" --creator SP --source TEST --json > "$WORK/rn_dry.json"
[[ "$(field "$WORK/rn_dry.json" "['renamed']")" == "3" ]] || { echo "FAIL: rename dry count ($(cat "$WORK/rn_dry.json"))" >&2; exit 1; }
[[ -f "$LIB/ui/menu_click_confirm.wav" ]] || { echo "FAIL: dry rename moved a file" >&2; exit 1; }
cp "$FX/click.wav" "$LIB/ui/UIClick_Menu Click Confirm_SP_TEST.wav"   # collision for the first
"$BIN" library update "$LIB" --quiet > /dev/null
"$BIN" library rename "$LIB" --creator SP --source TEST --apply --json > "$WORK/rn.json"
[[ "$(field "$WORK/rn.json" "['renamed']")" == "2" ]] || { echo "FAIL: rename apply count ($(cat "$WORK/rn.json"))" >&2; exit 1; }
[[ "$(field "$WORK/rn.json" "['skipped']")" -ge 1 ]] || { echo "FAIL: collision should be skipped" >&2; exit 1; }
[[ -f "$LIB/amb/WINDTonl_Desert Wind Howl_SP_TEST.flac" ]] || { echo "FAIL: renamed file missing" >&2; exit 1; }
[[ -f "$LIB/ui/menu_click_confirm.wav" ]] || { echo "FAIL: colliding file should stay" >&2; exit 1; }
"$BIN" library show "$LIB/amb/WINDTonl_Desert Wind Howl_SP_TEST.flac" --json > "$WORK/rn_show.json"
[[ "$(field "$WORK/rn_show.json" "['annotation']['source']")" == "human" ]] || { echo "FAIL: annotation did not follow the rename" >&2; exit 1; }
"$BIN" library update "$LIB" --json > "$WORK/u2.json"
[[ "$(field "$WORK/u2.json" "['update']['analyzed']")" == "0" ]] || { echo "FAIL: rename should leave nothing to re-analyze" >&2; exit 1; }
[[ "$(field "$WORK/u2.json" "['update']['missing']")" == "0" ]] || { echo "FAIL: rename left missing rows" >&2; exit 1; }

echo "library_embed_roundtrip.sh: PASS"
