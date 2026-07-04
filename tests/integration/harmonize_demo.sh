#!/usr/bin/env bash
# The M9 closed loop (extension §7): an off-palette sound is proposed a recipe, harmonized,
# and the result lints clean against the same baseline.
#
# Deviation (approved, see NOTES.md M9): the baseline is a scan of the FULL v1 fixture set,
# not darkset alone. Against darkset the demo is numerically impossible: darkset's per-dim
# sigma sits at the 0.02 floor, so no clamped +-12 dB tier-one op can pull any foreign sound
# within 2.5 sigma, and a noise-based fixture offends ton01/jitter01 (z ~ -50), which the
# extension itself scopes out of tier one.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT_DIR"

BIN="./build/cli/soundpalette"
GEN="./build/tools/genfixtures/genfixtures"
FIXTURES="tests/golden/fixtures"
EXTRA="fixtures_m9"

if [[ ! -x "$BIN" || ! -x "$GEN" ]]; then
    echo "harmonize_demo.sh: missing binaries (build first)" >&2
    exit 2
fi
if [[ ! -f "$EXTRA/fixable_outlier.wav" ]]; then
    "$GEN" "$FIXTURES" --extra "$EXTRA" >/dev/null
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

BASELINE="$WORK/baseline.json"
"$BIN" scan "$FIXTURES" --out "$BASELINE" --quiet

# Precondition: the fixture is genuinely off-palette.
set +e
mkdir "$WORK/pre" && cp "$EXTRA/fixable_outlier.wav" "$WORK/pre/"
"$BIN" lint "$WORK/pre" --baseline "$BASELINE" --quiet > /dev/null
if [[ $? -ne 1 ]]; then
    echo "harmonize_demo.sh: FAIL (fixable_outlier is not an outlier pre-harmonize)" >&2
    exit 1
fi
set -e

# Harmonize: must exit 0 and write the WAV + recipe sidecar.
if ! "$BIN" harmonize "$EXTRA/fixable_outlier.wav" --baseline "$BASELINE" \
        --out-dir "$WORK/harmonized" > "$WORK/harmonize.out"; then
    echo "harmonize_demo.sh: FAIL (harmonize did not exit 0)" >&2
    cat "$WORK/harmonize.out" >&2
    exit 1
fi
WAV="$WORK/harmonized/fixable_outlier.harmonized.wav"
SIDECAR="$WORK/harmonized/fixable_outlier.harmonized.recipe.json"
if [[ ! -s "$WAV" || ! -s "$SIDECAR" ]]; then
    echo "harmonize_demo.sh: FAIL (missing output WAV or recipe sidecar)" >&2
    exit 1
fi

# The harmonized file lints clean against the same baseline.
mkdir "$WORK/post" && cp "$WAV" "$WORK/post/"
if ! "$BIN" lint "$WORK/post" --baseline "$BASELINE" > "$WORK/lint.out"; then
    echo "harmonize_demo.sh: FAIL (harmonized file still off-palette)" >&2
    cat "$WORK/lint.out" >&2
    exit 1
fi

# Report sanity: max_z_after <= 2.5, iterations <= 3.
python3 - "$SIDECAR" <<'EOF'
import json, sys
r = json.load(open(sys.argv[1]))["result"]
assert r["max_z_after"] <= 2.5, f"max_z_after {r['max_z_after']} > 2.5"
assert r["iterations"] <= 3, f"iterations {r['iterations']} > 3"
EOF

echo "harmonize_demo.sh: PASS"
exit 0
