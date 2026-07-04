#!/usr/bin/env bash
# start watch; copy a new WAV in; manifest contains its path within 3 s; clean shutdown (§11).
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT_DIR"

BIN="./build/cli/soundpalette"
FIXTURES="tests/golden/fixtures"

if [[ ! -x "$BIN" ]]; then
    echo "watch_update.sh: missing $BIN (build first)" >&2
    exit 2
fi
if [[ ! -f "$FIXTURES/sine440_1s.wav" || ! -f "$FIXTURES/click.wav" ]]; then
    echo "watch_update.sh: missing fixtures (run genfixtures first)" >&2
    exit 2
fi

WORK="$(mktemp -d)"
WATCH_PID=""
cleanup() {
    if [[ -n "$WATCH_PID" ]] && kill -0 "$WATCH_PID" 2>/dev/null; then
        kill -9 "$WATCH_PID" 2>/dev/null || true
    fi
    rm -rf "$WORK"
}
trap cleanup EXIT

WATCHED="$WORK/watched"
MANIFEST="$WORK/palette.json"
mkdir "$WATCHED"
cp "$FIXTURES/click.wav" "$WATCHED/"

"$BIN" watch "$WATCHED" --out "$MANIFEST" --interval-ms 100 --quiet &
WATCH_PID=$!

# Wait for the initial manifest (up to 5 s).
for _ in $(seq 1 50); do
    [[ -f "$MANIFEST" ]] && break
    sleep 0.1
done
if [[ ! -f "$MANIFEST" ]]; then
    echo "watch_update.sh: FAIL (initial manifest never appeared)" >&2
    exit 1
fi

# Drop a new WAV in; its path must appear in the manifest within 3 s.
cp "$FIXTURES/sine440_1s.wav" "$WATCHED/newcomer.wav"

FOUND=0
for _ in $(seq 1 30); do
    if grep -q '"newcomer.wav"' "$MANIFEST" 2>/dev/null; then
        FOUND=1
        break
    fi
    sleep 0.1
done
if [[ "$FOUND" -ne 1 ]]; then
    echo "watch_update.sh: FAIL (newcomer.wav not in manifest within 3 s)" >&2
    cat "$MANIFEST" >&2 || true
    exit 1
fi

# Clean shutdown: SIGTERM must be caught and exit 0.
kill -TERM "$WATCH_PID"
set +e
wait "$WATCH_PID"
STATUS=$?
set -e
WATCH_PID=""
if [[ "$STATUS" -ne 0 ]]; then
    echo "watch_update.sh: FAIL (watch exited $STATUS on SIGTERM, expected clean 0)" >&2
    exit 1
fi

# The atomic-rename discipline must never leave a live temp file behind.
if [[ -f "$MANIFEST.tmp" ]]; then
    echo "watch_update.sh: FAIL (leftover $MANIFEST.tmp after shutdown)" >&2
    exit 1
fi

echo "watch_update.sh: PASS"
exit 0
