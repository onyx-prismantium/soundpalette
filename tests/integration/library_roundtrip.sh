#!/usr/bin/env bash
# Extension-4 §11: init -> update -> offline classify -> search -> export; export must be
# byte-identical to a plain scan of the same tree; a curated name list must classify exactly as
# the golden says (tests/golden/library_offline.json). Model-free.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT_DIR"

BIN="./build/cli/soundpalette"
FX="tests/golden/fixtures"
GOLDEN="tests/golden/library_offline.json"
if [[ ! -x "$BIN" || ! -d "$FX" ]]; then
    echo "library_roundtrip.sh: missing binary or fixtures (build + genfixtures first)" >&2
    exit 2
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
LIB="$WORK/lib"
mkdir -p "$LIB"

# Build the tree from the golden's path list (every path gets a fixture copy).
python3 - "$GOLDEN" "$LIB" "$FX" <<'PY'
import json, os, shutil, sys
golden, lib, fx = sys.argv[1:4]
paths = json.load(open(golden))["files"]
for i, rel in enumerate(paths):
    dst = os.path.join(lib, rel)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    src = os.path.join(fx, "click.wav" if i % 2 == 0 else "bright_outlier.wav")
    shutil.copyfile(src, dst)
PY

"$BIN" library init "$LIB" --quiet > "$WORK/init.txt"
grep -q "library created" "$WORK/init.txt"

# Offline classification equals the golden exactly.
"$BIN" library search "$LIB" --limit 0 --json > "$WORK/search.json"
python3 - "$GOLDEN" "$WORK/search.json" <<'PY'
import json, sys
golden = json.load(open(sys.argv[1]))
got = {r["path"]: (r["annotation"] or {}).get("cat_id", "") for r in json.load(open(sys.argv[2]))["results"]}
bad = []
for path, expect in golden["files"].items():
    actual = got.get(path)
    if actual != (expect or ""):
        bad.append((path, expect, actual))
if bad:
    for b in bad:
        print("MISMATCH %s: expected %r got %r" % b)
    sys.exit(1)
print("offline classification matches golden (%d files)" % len(golden["files"]))
PY

# Export == scan.
"$BIN" library export "$LIB" --manifest "$WORK/export.json" > /dev/null
"$BIN" scan "$LIB" --out "$WORK/scan.json" --quiet
cmp "$WORK/export.json" "$WORK/scan.json" || { echo "FAIL: export differs from scan" >&2; exit 1; }

# Search hits, filters, human edit + lock survive an update, second update analyzes nothing.
"$BIN" library search "$LIB" pistol | grep -q "GUNPis" || { echo "FAIL: search pistol" >&2; exit 1; }
"$BIN" library search "$LIB" --category "USER INTERFACE" | grep -q "UIClick" || { echo "FAIL: category filter" >&2; exit 1; }
"$BIN" library set "$LIB/misc/zz_unknown_x7.wav" --catid TOONBoing --description "A cartoon boing." > /dev/null
"$BIN" library set "$LIB/foley/coin_pickup.wav" --catid OBJCoin --description "A single coin pickup." > /dev/null
"$BIN" library classify "$LIB" --force | grep -q "1 locked" || { echo "FAIL: lock not honored" >&2; exit 1; }
"$BIN" library update "$LIB" --json > "$WORK/u2.json"
grep -q '"analyzed": 0' "$WORK/u2.json" || { echo "FAIL: second update re-analyzed" >&2; exit 1; }
"$BIN" library show "$LIB/misc/zz_unknown_x7.wav" | grep -q "human \[locked\]" || { echo "FAIL: show" >&2; exit 1; }
"$BIN" library stats "$LIB" | grep -q "CARTOON" || { echo "FAIL: stats" >&2; exit 1; }
"$BIN" library export "$LIB" --manifest "$WORK/guns.json" --category GUNS > /dev/null
python3 -c "import json,sys; m=json.load(open('$WORK/guns.json')); sys.exit(0 if m['file_count']==2 else 1)" || { echo "FAIL: filtered export" >&2; exit 1; }

echo "library_roundtrip.sh: PASS"
