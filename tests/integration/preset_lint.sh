#!/usr/bin/env bash
# Designed genre presets: list shows all four, export produces a parseable .sppal.json that
# `profile show` reports as designed, and lint consumes it like any --profile. Deterministic:
# exporting twice gives identical bytes.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT_DIR"

BIN="./build/cli/soundpalette"
GEN="./build/tools/genfixtures/genfixtures"

if [[ ! -x "$BIN" || ! -x "$GEN" ]]; then
    echo "preset_lint.sh: missing binaries (build first)" >&2
    exit 2
fi
if [[ ! -d "fixtures_m10/catfx" ]]; then
    "$GEN" tests/golden/fixtures --profile-set fixtures_m10 >/dev/null
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

LIST="$("$BIN" profile preset list)"
for slug in sci-fi horror fantasy retro-8bit; do
    if ! grep -q "^$slug" <<<"$LIST"; then
        echo "preset_lint.sh: '$slug' missing from preset list" >&2
        exit 1
    fi
done

"$BIN" profile preset export fantasy --out "$WORK/a.sppal.json" >/dev/null
"$BIN" profile preset export fantasy --out "$WORK/b.sppal.json" >/dev/null
if ! cmp -s "$WORK/a.sppal.json" "$WORK/b.sppal.json"; then
    echo "preset_lint.sh: preset export is not deterministic" >&2
    exit 1
fi

SHOW="$("$BIN" profile show "$WORK/a.sppal.json")"
if ! grep -q "from designed" <<<"$SHOW"; then
    echo "preset_lint.sh: profile show does not report designed provenance" >&2
    exit 1
fi

# Lint must run and produce full per-file JSON against the exported preset; catfx is synthetic
# so we only assert the pipeline, not a flag rate.
"$BIN" profile preset export retro-8bit --out "$WORK/retro.sppal.json" >/dev/null
"$BIN" lint fixtures_m10/catfx --profile "$WORK/retro.sppal.json" --json --all \
    > "$WORK/all.json" || true
python3 - "$WORK/all.json" <<'EOF'
import json, sys
j = json.load(open(sys.argv[1]))
assert len(j["files"]) == 16, f'expected 16 linted files, got {len(j["files"])}'
EOF

echo "preset_lint.sh: OK"
