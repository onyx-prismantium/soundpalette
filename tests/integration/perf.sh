#!/usr/bin/env bash
# scan perf/ --threads 4 completes in <= 10 s wall on the 200-file set (§11).
#
# Uses its own scratch fixture directory (not tests/golden/fixtures) so the 200 perf files never
# become part of the committed golden manifest.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT_DIR"

BIN="./build/cli/soundpalette"
GENFIXTURES="./build/tools/genfixtures/genfixtures"

if [[ ! -x "$BIN" || ! -x "$GENFIXTURES" ]]; then
    echo "perf.sh: missing $BIN or $GENFIXTURES (build first)" >&2
    exit 2
fi

SCRATCH="$(mktemp -d)"
trap 'rm -rf "$SCRATCH"' EXIT

"$GENFIXTURES" "$SCRATCH" --perf200 > /dev/null

START=$(date +%s.%N)
"$BIN" scan "$SCRATCH/perf" --threads 4 --quiet --out /dev/null
END=$(date +%s.%N)

ELAPSED=$(echo "$END - $START" | bc)
echo "perf.sh: scanned perf/ (200 files) in ${ELAPSED}s"

PASS=$(echo "$ELAPSED <= 10.0" | bc)
if [[ "$PASS" == "1" ]]; then
    echo "perf.sh: PASS"
    exit 0
else
    echo "perf.sh: FAIL (${ELAPSED}s > 10s)" >&2
    exit 1
fi
