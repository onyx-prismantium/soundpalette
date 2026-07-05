#!/usr/bin/env bash
# The per-category value proposition (extension-2 §8): catfx lints clean against its own
# category profile; a ui tick misplaced into combat/ is flagged with cat=combat; --json --all
# parses and covers all 17 files. Also the cross-surface agreement spot-check (§10.2): the
# CLI's per-file table is what MCP get_deviations wraps, so its numbers are compared there.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT_DIR"

BIN="./build/cli/soundpalette"
GEN="./build/tools/genfixtures/genfixtures"

if [[ ! -x "$BIN" || ! -x "$GEN" ]]; then
    echo "category_lint.sh: missing binaries (build first)" >&2
    exit 2
fi
if [[ ! -d "fixtures_m10/catfx" ]]; then
    "$GEN" tests/golden/fixtures --profile-set fixtures_m10 >/dev/null
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

PROFILE="$WORK/catfx.sppal.json"
"$BIN" profile create fixtures_m10/catfx --name catfx \
    --category 'ui=ui/**,**/ui_*' --category 'combat=combat/**' --out "$PROFILE"

# 1. Clean tree lints clean.
if ! "$BIN" lint fixtures_m10/catfx --profile "$PROFILE" > "$WORK/clean.out"; then
    echo "category_lint.sh: FAIL (clean catfx did not lint clean)" >&2
    cat "$WORK/clean.out" >&2
    exit 1
fi

# 2. Misplaced ui tick inside combat/ is flagged with its resolved category.
cp -r fixtures_m10/catfx "$WORK/tree"
cp "$WORK/tree/ui/ui_00.wav" "$WORK/tree/combat/misplaced.wav"

set +e
"$BIN" lint "$WORK/tree" --profile "$PROFILE" > "$WORK/mixed.out"
STATUS=$?
set -e
if [[ "$STATUS" -ne 1 ]]; then
    echo "category_lint.sh: FAIL (misplaced lint exited $STATUS, expected 1)" >&2
    cat "$WORK/mixed.out" >&2
    exit 1
fi
grep -q "misplaced.wav" "$WORK/mixed.out" || { echo "FAIL: no misplaced.wav" >&2; exit 1; }
grep -q "cat=combat" "$WORK/mixed.out" || { echo "FAIL: no cat=combat" >&2; exit 1; }

# 3. --json --all parses and has 17 entries.
set +e
"$BIN" lint "$WORK/tree" --profile "$PROFILE" --json --all > "$WORK/all.json"
set -e
python3 - "$WORK/all.json" <<'EOF'
import json, sys
j = json.load(open(sys.argv[1]))
assert len(j["files"]) == 17, f"expected 17 entries, got {len(j['files'])}"
assert any(f["path"] == "combat/misplaced.wav" and f["band"] == "red" for f in j["files"])
assert all("z" in f and len(f["z"]) == 8 for f in j["files"])
EOF

echo "category_lint.sh: PASS"
exit 0
