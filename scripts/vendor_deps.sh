#!/usr/bin/env bash
# Refresh the checked-in header-only dependencies (see third_party/README.md).
set -euo pipefail
cd "$(dirname "$0")/.."

JSON_VERSION=v3.11.3
CATCH_VERSION=v3.5.4
HTTPLIB_VERSION=v0.15.3

mkdir -p third_party/{nlohmann,catch2,httplib}
curl -fsSL -o third_party/nlohmann/json.hpp \
  "https://raw.githubusercontent.com/nlohmann/json/${JSON_VERSION}/single_include/nlohmann/json.hpp"
curl -fsSL -o third_party/catch2/catch_amalgamated.hpp \
  "https://raw.githubusercontent.com/catchorg/Catch2/${CATCH_VERSION}/extras/catch_amalgamated.hpp"
curl -fsSL -o third_party/catch2/catch_amalgamated.cpp \
  "https://raw.githubusercontent.com/catchorg/Catch2/${CATCH_VERSION}/extras/catch_amalgamated.cpp"
curl -fsSL -o third_party/httplib/httplib.h \
  "https://raw.githubusercontent.com/yhirose/cpp-httplib/${HTTPLIB_VERSION}/httplib.h"

echo "vendored json ${JSON_VERSION}, catch2 ${CATCH_VERSION}, cpp-httplib ${HTTPLIB_VERSION}"
