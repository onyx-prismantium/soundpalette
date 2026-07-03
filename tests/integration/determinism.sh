#!/usr/bin/env bash
# Two consecutive scans produce identical bytes (§11).
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT_DIR"

BIN="./build/cli/soundpalette"
FIXTURES="tests/golden/fixtures"

if [[ ! -x "$BIN" ]]; then
    echo "determinism.sh: missing $BIN (build first)" >&2
    exit 2
fi

OUT1="$(mktemp)"
OUT2="$(mktemp)"
trap 'rm -f "$OUT1" "$OUT2"' EXIT

"$BIN" scan "$FIXTURES" --out "$OUT1" --quiet
"$BIN" scan "$FIXTURES" --out "$OUT2" --quiet

if cmp -s "$OUT1" "$OUT2"; then
    echo "determinism.sh: PASS"
    exit 0
else
    echo "determinism.sh: FAIL (two scans of the same directory differ)" >&2
    diff -u "$OUT1" "$OUT2" | head -50 >&2
    exit 1
fi
