# Multi-stage build for a single deployable EspressoLab instance:
# the native REST tool_server plus the built React dashboard, served
# together behind nginx. Matches scripts/dev.sh's shape (server on
# 127.0.0.1:8734, web talking to it over /api) but as one container
# with nginx as the public front door instead of the Vite dev server.

# --- Stage 1: native C++ build (offline, vendored deps per CLAUDE.md) ---
# Ubuntu 24.04 ships GCC 13 by default; GCC 12 (Debian bookworm's default)
# doesn't fully implement <format>, which engine/artifact_io/hashing.cpp needs.
FROM ubuntu:24.04 AS native-build
RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake g++ make ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DESPRESSOLAB_BUILD_TESTS=OFF \
    && cmake --build build -j"$(nproc)" --target espressolab_server

# --- Stage 2: web build ---
FROM node:20-bookworm-slim AS web-build
WORKDIR /src/web
COPY web/package*.json ./
RUN npm ci
COPY web/ ./
RUN npm run build

# --- Stage 3: runtime ---
# Must match native-build's glibc/libstdc++ ABI, hence also Ubuntu 24.04.
FROM ubuntu:24.04 AS runtime
RUN apt-get update && apt-get install -y --no-install-recommends \
    nginx ca-certificates curl \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=native-build /src/build/apps/espressolab_server/espressolab_server /app/espressolab_server
COPY assets /app/assets
COPY espresso_real_world_refs /app/espresso_real_world_refs
COPY --from=web-build /src/web/dist /app/web/dist
COPY deploy/nginx.conf /etc/nginx/sites-enabled/default
COPY deploy/entrypoint.sh /app/entrypoint.sh
RUN chmod +x /app/entrypoint.sh

EXPOSE 8080
CMD ["/app/entrypoint.sh"]
