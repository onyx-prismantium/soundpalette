#!/usr/bin/env bash
# Non-destructive guarantee (extension §7): a full harmonize run never modifies any source
# file, and every recipe sidecar's source.sha256 matches its input.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT_DIR"

BIN="./build/cli/soundpalette"
GEN="./build/tools/genfixtures/genfixtures"
FIXTURES="tests/golden/fixtures"
EXTRA="fixtures_m9"

if [[ ! -x "$BIN" || ! -x "$GEN" ]]; then
    echo "provenance.sh: missing binaries (build first)" >&2
    exit 2
fi
if [[ ! -f "$EXTRA/fixable_outlier.wav" ]]; then
    "$GEN" "$FIXTURES" --extra "$EXTRA" >/dev/null
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# Work on a copy of darkset + fixable_outlier (extension §7).
SRC="$WORK/src"
mkdir -p "$SRC"
cp "$FIXTURES"/darkset/*.wav "$SRC/"
cp "$EXTRA/fixable_outlier.wav" "$SRC/"

BASELINE="$WORK/baseline.json"
"$BIN" scan "$FIXTURES" --out "$BASELINE" --quiet

( cd "$SRC" && sha256sum *.wav | sort ) > "$WORK/before.sha"

set +e
"$BIN" harmonize "$SRC" --baseline "$BASELINE" --out-dir "$WORK/harmonized" \
    > "$WORK/harmonize.out"
STATUS=$?
set -e
if [[ "$STATUS" -ne 0 && "$STATUS" -ne 1 ]]; then
    echo "provenance.sh: FAIL (harmonize errored: exit $STATUS)" >&2
    cat "$WORK/harmonize.out" >&2
    exit 1
fi

( cd "$SRC" && sha256sum *.wav | sort ) > "$WORK/after.sha"
if ! cmp -s "$WORK/before.sha" "$WORK/after.sha"; then
    echo "provenance.sh: FAIL (source files were modified)" >&2
    diff "$WORK/before.sha" "$WORK/after.sha" >&2
    exit 1
fi

# Every sidecar's source.sha256 matches the actual input hash.
python3 - "$SRC" "$WORK/harmonized" <<'EOF'
import hashlib, json, pathlib, sys
src, harm = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
sidecars = list(harm.rglob("*.recipe.json"))
assert sidecars, "no recipe sidecars written"
for sc in sidecars:
    r = json.load(open(sc))
    rel = r["source"]["path"]
    actual = hashlib.sha256((src / rel).read_bytes()).hexdigest()
    assert r["source"]["sha256"] == actual, f"sha mismatch for {rel}"
EOF

echo "provenance.sh: PASS"
exit 0
