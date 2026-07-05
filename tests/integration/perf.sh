#!/usr/bin/env bash
# scan perf/ --threads 4 on the 200-file set (§11 + extension-3 §8): with the psychoacoustic
# block on (the default) <= 45 s wall; with --no-psycho the original <= 10 s bound holds.
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
echo "perf.sh: scanned perf/ (200 files, psycho on) in ${ELAPSED}s"
if [[ "$(echo "$ELAPSED <= 45.0" | bc)" != "1" ]]; then
    echo "perf.sh: FAIL (psycho on: ${ELAPSED}s > 45s)" >&2
    exit 1
fi

START=$(date +%s.%N)
"$BIN" scan "$SCRATCH/perf" --threads 4 --quiet --no-psycho --out /dev/null
END=$(date +%s.%N)
ELAPSED=$(echo "$END - $START" | bc)
echo "perf.sh: scanned perf/ (200 files, --no-psycho) in ${ELAPSED}s"
if [[ "$(echo "$ELAPSED <= 10.0" | bc)" != "1" ]]; then
    echo "perf.sh: FAIL (--no-psycho: ${ELAPSED}s > 10s)" >&2
    exit 1
fi
echo "perf.sh: PASS"
