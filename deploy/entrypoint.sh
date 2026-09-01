#!/usr/bin/env bash
set -euo pipefail

/app/espressolab_server --assets /app/assets --references /app/espresso_real_world_refs --port 8734 &
SERVER_PID=$!
trap 'kill "$SERVER_PID" 2>/dev/null || true' EXIT

# Wait for the native server to bind before nginx starts proxying to it.
for _ in $(seq 1 50); do
    curl -sf http://127.0.0.1:8734/api/v1/health >/dev/null 2>&1 && break
    sleep 0.2
done

nginx -g "daemon off;"
