#!/usr/bin/env bash
# export-svg --profile halos (extension-2 §8): well-formed, exactly one data-dev="red",
# deterministic across two runs.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT_DIR"

BIN="./build/cli/soundpalette"
FIXTURES="tests/golden/fixtures"

if [[ ! -x "$BIN" ]]; then
    echo "svg_halos.sh: missing $BIN (build first)" >&2
    exit 2
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# Tree = darkset + bright_outlier; profile from darkset alone so exactly the outlier flags red.
mkdir "$WORK/tree"
cp "$FIXTURES"/darkset/*.wav "$WORK/tree/"
cp "$FIXTURES/bright_outlier.wav" "$WORK/tree/"

"$BIN" profile create "$FIXTURES/darkset" --name darkset --threshold 4.0 \
    --out "$WORK/darkset.sppal.json"
"$BIN" scan "$WORK/tree" --out "$WORK/tree.json" --quiet

"$BIN" export-svg "$WORK/tree.json" --out "$WORK/s1.svg" --profile "$WORK/darkset.sppal.json"
"$BIN" export-svg "$WORK/tree.json" --out "$WORK/s2.svg" --profile "$WORK/darkset.sppal.json"

xmllint --noout "$WORK/s1.svg"

RED_COUNT="$(grep -o 'data-dev="red"' "$WORK/s1.svg" | wc -l)"
if [[ "$RED_COUNT" -ne 1 ]]; then
    echo "svg_halos.sh: FAIL (expected exactly 1 red halo, got $RED_COUNT)" >&2
    exit 1
fi

SHA1="$(sha256sum "$WORK/s1.svg" | cut -d' ' -f1)"
SHA2="$(sha256sum "$WORK/s2.svg" | cut -d' ' -f1)"
if [[ "$SHA1" != "$SHA2" ]]; then
    echo "svg_halos.sh: FAIL (two exports differ)" >&2
    exit 1
fi

echo "svg_halos.sh: PASS (1 red halo, deterministic sha256=$SHA1)"
exit 0
