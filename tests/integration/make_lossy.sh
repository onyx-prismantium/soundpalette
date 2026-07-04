#!/usr/bin/env bash
# Transcodes sine440_1s.wav to .flac/.ogg/.mp3 for decoder coverage (PLAN.md §11).
set -euo pipefail

FIXTURES_DIR="${1:-tests/golden/fixtures}"
SRC="${FIXTURES_DIR}/sine440_1s.wav"

if [[ ! -f "$SRC" ]]; then
    echo "make_lossy.sh: missing $SRC (run genfixtures first)" >&2
    exit 2
fi

ffmpeg -y -loglevel error -i "$SRC" -ar 48000 -ac 1 -c:a flac "${FIXTURES_DIR}/sine440_1s.flac"
ffmpeg -y -loglevel error -i "$SRC" -ar 48000 -ac 1 -c:a libvorbis -q:a 5 "${FIXTURES_DIR}/sine440_1s.ogg"
ffmpeg -y -loglevel error -i "$SRC" -ar 48000 -ac 1 -c:a libmp3lame -b:a 192k "${FIXTURES_DIR}/sine440_1s.mp3"

# ffmpeg/libvorbis picks a random Ogg logical-bitstream serial per encode; normalize it so the
# .ogg fixture is byte-identical across regenerations (determinism §1 rule 5).
# Windows Git Bash ships only a `python` shim, so fall back to it when `python3` is absent.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PYTHON="$(command -v python3 || command -v python)"
"$PYTHON" "${SCRIPT_DIR}/fix_ogg_serial.py" "${FIXTURES_DIR}/sine440_1s.ogg"

echo "make_lossy.sh: wrote flac/ogg/mp3 next to $SRC"
