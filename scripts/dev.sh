#!/usr/bin/env bash
# Run the tool server and the dashboard dev server side by side.
set -euo pipefail
cd "$(dirname "$0")/.."

[ -x build/apps/espressolab_server/espressolab_server ] || ./scripts/build.sh Release
[ -d web/node_modules ] || (cd web && npm install)

./build/apps/espressolab_server/espressolab_server --assets assets \
  --references espresso_real_world_refs --port 8734 &
SERVER_PID=$!
trap 'kill "$SERVER_PID" 2>/dev/null || true' EXIT

cd web && npm run dev
