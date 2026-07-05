#!/usr/bin/env bash
# Extension-3 M13 gate: the C++ CLI's psychoacoustic numbers vs the committed MoSQITo
# reference, within the §5 tolerances. Prints a per-file table.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT_DIR"

BIN="./build/cli/soundpalette"
GEN="./build/tools/genfixtures/genfixtures"
REF="tests/golden/psycho_reference.json"

if [[ ! -x "$BIN" || ! -x "$GEN" ]]; then
    echo "psycho_oracle_match.sh: missing binaries (build first)" >&2
    exit 2
fi
if [[ ! -d "fixtures_psycho" ]]; then
    "$GEN" tests/golden/fixtures --psycho fixtures_psycho >/dev/null
fi

FILES=(
    fixtures_psycho/tone1k_40db.wav fixtures_psycho/tone1k_50db.wav
    fixtures_psycho/tone1k_60db.wav fixtures_psycho/am70_60db.wav
    fixtures_psycho/am4_60db.wav fixtures_psycho/nbnoise1k_60db.wav
    tests/golden/fixtures/sine997_cal.wav tests/golden/fixtures/noise_white_1s.wav
    tests/golden/fixtures/click.wav tests/golden/fixtures/decay_t60.wav
    tests/golden/fixtures/darkset/dark_00.wav tests/golden/fixtures/bright_outlier.wav
)

"$BIN" psycho "${FILES[@]}" > /tmp/psycho_ours.json

python3 - /tmp/psycho_ours.json "$REF" <<'EOF'
import json, sys, os

ours = json.load(open(sys.argv[1]))
ref = json.load(open(sys.argv[2]))["files"]

STATIONARY = {"tone1k_40db.wav", "tone1k_50db.wav", "tone1k_60db.wav",
              "nbnoise1k_60db.wav", "sine997_cal.wav", "noise_white_1s.wav"}
TRANSIENT = {"click.wav", "decay_t60.wav", "darkset/dark_00.wav"}
AM = {"am70_60db.wav", "am4_60db.wav"}

def refname(path):
    p = path.replace("tests/golden/fixtures/", "").replace("fixtures_psycho/", "")
    return p

fails = []
print(f"{'file':<28} {'metric':<12} {'ours':>9} {'oracle':>9} {'tol':>10} {'status':>7}")
for path, vals in ours.items():
    name = refname(path)
    r = ref[name]
    rows = []
    if name in STATIONARY:
        # The ±5 % stationary-loudness gate proper runs against the stationary method in
        # tests/unit/test_psycho.cpp (psycho/oracle_stationary). Here N5 of a steady signal
        # doubles as a public-API check — except for the 1 s noise file, whose N5 estimate is
        # still inside the loudness-time-function convergence window (real behaviour, not an
        # error; the unit test covers its stationary value).
        if name != "noise_white_1s.wav":
            rows.append(("n5~zwst", vals["sones_n5"], r["loudness_zwst_sone"], 0.05, "rel"))
        rows.append(("sharpness", vals["sharpness_acum"], r["sharpness_din_acum"], 0.05, "rel"))
    if name in TRANSIENT:
        rows.append(("n5", vals["sones_n5"], r["loudness_zwtv_n5_sone"], 0.10, "rel"))
    if name in AM:
        tol = max(0.15 * r["roughness_dw_asper"], 0.05)
        rows.append(("roughness", vals["roughness_asper"], r["roughness_dw_asper"], tol, "abs"))
    for metric, mine, oracle, tol, kind in rows:
        if kind == "rel":
            ok = abs(mine / oracle - 1.0) <= tol
            tol_s = f"±{tol*100:.0f}%"
        else:
            ok = abs(mine - oracle) <= tol
            tol_s = f"±{tol:.3f}"
        print(f"{name:<28} {metric:<12} {mine:>9.4f} {oracle:>9.4f} {tol_s:>10} "
              f"{'PASS' if ok else 'FAIL':>7}")
        if not ok:
            fails.append((name, metric))

bright = ours["tests/golden/fixtures/bright_outlier.wav"]["sharpness_acum"]
dark = ours["tests/golden/fixtures/darkset/dark_00.wav"]["sharpness_acum"]
ok = bright > dark
print(f"{'bright vs dark':<28} {'direction':<12} {bright:>9.4f} {dark:>9.4f} {'>':>10} "
      f"{'PASS' if ok else 'FAIL':>7}")
if not ok:
    fails.append(("direction", "sharpness"))

if fails:
    print(f"psycho_oracle_match.sh: {len(fails)} FAILED rows", file=sys.stderr)
    sys.exit(1)
print("psycho_oracle_match.sh: OK")
EOF
