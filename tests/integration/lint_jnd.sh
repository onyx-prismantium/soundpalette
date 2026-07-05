#!/usr/bin/env bash
# lint v2 text (extension-3 §7): deviations phrased in JNDs alongside z; names the outlier;
# exit code 1 unchanged.
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT_DIR"
BIN="./build/cli/soundpalette"
[[ -x "$BIN" ]] || { echo "lint_jnd.sh: build first" >&2; exit 2; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
mkdir "$WORK/tree"
cp tests/golden/fixtures/darkset/*.wav "$WORK/tree/"
cp tests/golden/fixtures/bright_outlier.wav "$WORK/tree/"
"$BIN" scan tests/golden/fixtures/darkset --out "$WORK/dark.json" --quiet

set +e
"$BIN" lint "$WORK/tree" --baseline "$WORK/dark.json" --threshold 4.0 > "$WORK/out.txt"
STATUS=$?
set -e
[[ "$STATUS" -eq 1 ]] || { echo "FAIL: lint exited $STATUS, expected 1" >&2; exit 1; }
grep -q "bright_outlier.wav" "$WORK/out.txt" || { echo "FAIL: outlier not named" >&2; exit 1; }
grep -q "JND" "$WORK/out.txt" || { echo "FAIL: no JND phrasing" >&2; cat "$WORK/out.txt" >&2; exit 1; }
grep -q "acum" "$WORK/out.txt" || { echo "FAIL: no unit in text" >&2; exit 1; }
echo "lint_jnd.sh: OK"
