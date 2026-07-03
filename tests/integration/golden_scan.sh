#!/usr/bin/env bash
# scan fixtures --no-meta output equals committed tests/golden/palette.json byte-for-byte (§11).
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT_DIR"

BIN="./build/cli/soundpalette"
FIXTURES="tests/golden/fixtures"
GOLDEN="tests/golden/palette.json"

if [[ ! -x "$BIN" ]]; then
    echo "golden_scan.sh: missing $BIN (build first)" >&2
    exit 2
fi
if [[ ! -f "$GOLDEN" ]]; then
    echo "golden_scan.sh: missing $GOLDEN" >&2
    exit 2
fi

OUT="$(mktemp)"
trap 'rm -f "$OUT"' EXIT

"$BIN" scan "$FIXTURES" --no-meta --out "$OUT" --quiet

if cmp -s "$GOLDEN" "$OUT"; then
    echo "golden_scan.sh: PASS (byte-identical)"
    exit 0
else
    echo "golden_scan.sh: FAIL (scan output differs from $GOLDEN)" >&2
    diff -u "$GOLDEN" "$OUT" | head -50 >&2
    exit 1
fi
