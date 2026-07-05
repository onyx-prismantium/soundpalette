#!/usr/bin/env bash
# describe v2 (extension-3 §7): the sentence carries perceptual units, and --json exposes the
# psycho block plus the fixed definitional anchor strings. On the 1-sone reference tone the
# reported loudness must be ~1.0 sone.
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT_DIR"
BIN="./build/cli/soundpalette"
GEN="./build/tools/genfixtures/genfixtures"
[[ -x "$BIN" && -x "$GEN" ]] || { echo "describe_units.sh: build first" >&2; exit 2; }
[[ -d fixtures_psycho ]] || "$GEN" tests/golden/fixtures --psycho fixtures_psycho >/dev/null

TEXT="$("$BIN" describe fixtures_psycho/tone1k_40db.wav)"
grep -q "sones" <<<"$TEXT" || { echo "FAIL: sentence lacks 'sones': $TEXT" >&2; exit 1; }

"$BIN" describe fixtures_psycho/tone1k_40db.wav --json > /tmp/desc_units.json
python3 - /tmp/desc_units.json <<'PYEOF'
import json, sys
j = json.load(open(sys.argv[1]))
n5 = j["psycho"]["sones_n5"]
assert abs(n5 - 1.0) <= 0.1, f"sones_n5 {n5} not ~1.0"
assert "1 sone" in j["anchors"] and "40 dB SPL" in j["anchors"]["1 sone"]
assert "sones" in j["sentence"]
PYEOF
echo "describe_units.sh: OK"
