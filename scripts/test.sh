#!/usr/bin/env bash
# Build and run the full Catch2 suite.
set -euo pipefail
cd "$(dirname "$0")/.."

./scripts/build.sh "${1:-Release}"
./build/tests/espressolab_tests "${@:2}"
