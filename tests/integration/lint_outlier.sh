#!/usr/bin/env bash
# baseline = scan of darkset/; lint of darkset + bright_outlier.wav exits 1 and names
# bright_outlier.wav; lint of darkset alone exits 0 (§11).
#
# Threshold: 4.0, not the CLI default 2.5 (§11 leaves the script's threshold open). At 2.5 the
# spec's own darkset self-lints with 4 outliers (max |z| = 3.73: tail01 clipping at the 1.5 s
# fixture length + attack-time variance of noise bursts); at 4.0 self-lint passes while
# bright_outlier.wav is still flagged at z = +49.96. See NOTES.md M4, approved deviation.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT_DIR"

BIN="./build/cli/soundpalette"
FIXTURES="tests/golden/fixtures"

if [[ ! -x "$BIN" ]]; then
    echo "lint_outlier.sh: missing $BIN (build first)" >&2
    exit 2
fi
if [[ ! -d "$FIXTURES/darkset" || ! -f "$FIXTURES/bright_outlier.wav" ]]; then
    echo "lint_outlier.sh: missing fixtures (run genfixtures first)" >&2
    exit 2
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

BASELINE="$WORK/baseline.json"
"$BIN" scan "$FIXTURES/darkset" --out "$BASELINE" --quiet

# 1. darkset alone against its own baseline: must pass (exit 0).
if ! "$BIN" lint "$FIXTURES/darkset" --baseline "$BASELINE" --threshold 4.0 > "$WORK/lint_clean.out"; then
    echo "lint_outlier.sh: FAIL (lint of darkset against its own baseline did not exit 0)" >&2
    cat "$WORK/lint_clean.out" >&2
    exit 1
fi
if ! grep -q "^PASS " "$WORK/lint_clean.out"; then
    echo "lint_outlier.sh: FAIL (clean lint did not print a PASS line)" >&2
    cat "$WORK/lint_clean.out" >&2
    exit 1
fi

# 2. darkset + bright_outlier.wav: must exit 1 and name bright_outlier.wav.
MIXED="$WORK/mixed"
mkdir "$MIXED"
cp "$FIXTURES"/darkset/*.wav "$MIXED/"
cp "$FIXTURES/bright_outlier.wav" "$MIXED/"

set +e
"$BIN" lint "$MIXED" --baseline "$BASELINE" --threshold 4.0 > "$WORK/lint_mixed.out"
STATUS=$?
set -e

if [[ "$STATUS" -ne 1 ]]; then
    echo "lint_outlier.sh: FAIL (mixed lint exited $STATUS, expected 1)" >&2
    cat "$WORK/lint_mixed.out" >&2
    exit 1
fi
if ! grep -q "^OUTLIER bright_outlier.wav " "$WORK/lint_mixed.out"; then
    echo "lint_outlier.sh: FAIL (mixed lint did not name bright_outlier.wav)" >&2
    cat "$WORK/lint_mixed.out" >&2
    exit 1
fi

echo "lint_outlier.sh: PASS"
exit 0
