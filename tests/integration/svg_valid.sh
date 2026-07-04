#!/usr/bin/env bash
# export-svg output passes xmllint --noout; two exports have equal sha256 (§11). Additionally
# compares against the committed golden SVG (tests/golden/sheet.svg), which is regenerated from
# the golden manifest and must stay byte-stable like the manifest itself (§3, §13.2).
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT_DIR"

BIN="./build/cli/soundpalette"
GOLDEN_MANIFEST="tests/golden/palette.json"
GOLDEN_SVG="tests/golden/sheet.svg"

if [[ ! -x "$BIN" ]]; then
    echo "svg_valid.sh: missing $BIN (build first)" >&2
    exit 2
fi
if [[ ! -f "$GOLDEN_MANIFEST" ]]; then
    echo "svg_valid.sh: missing $GOLDEN_MANIFEST" >&2
    exit 2
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

"$BIN" export-svg "$GOLDEN_MANIFEST" --out "$WORK/sheet1.svg" --columns 8
"$BIN" export-svg "$GOLDEN_MANIFEST" --out "$WORK/sheet2.svg" --columns 8

if ! xmllint --noout "$WORK/sheet1.svg"; then
    echo "svg_valid.sh: FAIL (SVG is not well-formed XML)" >&2
    exit 1
fi

SHA1="$(sha256sum "$WORK/sheet1.svg" | cut -d' ' -f1)"
SHA2="$(sha256sum "$WORK/sheet2.svg" | cut -d' ' -f1)"
if [[ "$SHA1" != "$SHA2" ]]; then
    echo "svg_valid.sh: FAIL (two exports differ: $SHA1 vs $SHA2)" >&2
    exit 1
fi

if [[ -f "$GOLDEN_SVG" ]]; then
    if ! cmp -s "$GOLDEN_SVG" "$WORK/sheet1.svg"; then
        echo "svg_valid.sh: FAIL (export differs from committed $GOLDEN_SVG)" >&2
        diff -u "$GOLDEN_SVG" "$WORK/sheet1.svg" | head -30 >&2
        exit 1
    fi
else
    echo "svg_valid.sh: note: $GOLDEN_SVG not committed yet, skipping golden comparison" >&2
fi

echo "svg_valid.sh: PASS (well-formed, deterministic sha256=$SHA1)"
exit 0
