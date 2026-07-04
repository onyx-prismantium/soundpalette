#!/usr/bin/env bash
# Builds mcp/ and runs the SDK-client integration suite over stdio (extension §7).
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT_DIR"

if [[ ! -x "./build/cli/soundpalette" && ! -f "./build/cli/soundpalette.exe" ]]; then
    echo "mcp_smoke.sh: missing build/cli/soundpalette (build first)" >&2
    exit 2
fi
if [[ ! -f "tests/golden/fixtures/sine440_1s.wav" ]]; then
    echo "mcp_smoke.sh: missing fixtures (run genfixtures first)" >&2
    exit 2
fi
if ! command -v node >/dev/null || ! command -v npm >/dev/null; then
    echo "mcp_smoke.sh: node/npm not found (extension §2 prerequisite)" >&2
    exit 2
fi

cd mcp
if [[ ! -d node_modules ]]; then
    npm ci
fi
npm run build
npm test

echo "mcp_smoke.sh: PASS"
exit 0
