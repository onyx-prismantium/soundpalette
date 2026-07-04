#!/usr/bin/env bash
# profile create on catfx with two categories -> show prints name + 8/8 counts; second create
# -> identical bytes (extension-2 §8).
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT_DIR"

BIN="./build/cli/soundpalette"
GEN="./build/tools/genfixtures/genfixtures"

if [[ ! -x "$BIN" || ! -x "$GEN" ]]; then
    echo "profile_roundtrip.sh: missing binaries (build first)" >&2
    exit 2
fi
if [[ ! -d "fixtures_m10/catfx" ]]; then
    "$GEN" tests/golden/fixtures --profile-set fixtures_m10 >/dev/null
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

"$BIN" profile create fixtures_m10/catfx --name catfx \
    --category 'ui=ui/**,**/ui_*' --category 'combat=combat/**' \
    --out "$WORK/a.sppal.json"
"$BIN" profile create fixtures_m10/catfx --name catfx \
    --category 'ui=ui/**,**/ui_*' --category 'combat=combat/**' \
    --out "$WORK/b.sppal.json"

if ! cmp -s "$WORK/a.sppal.json" "$WORK/b.sppal.json"; then
    echo "profile_roundtrip.sh: FAIL (two creates differ)" >&2
    exit 1
fi

SHOW="$("$BIN" profile show "$WORK/a.sppal.json")"
echo "$SHOW"
echo "$SHOW" | grep -q "profile catfx" || { echo "FAIL: name missing" >&2; exit 1; }
echo "$SHOW" | grep -q "category ui: 8 files" || { echo "FAIL: ui count" >&2; exit 1; }
echo "$SHOW" | grep -q "category combat: 8 files" || { echo "FAIL: combat count" >&2; exit 1; }

echo "profile_roundtrip.sh: PASS"
exit 0
