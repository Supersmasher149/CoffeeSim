#!/usr/bin/env bash
# Refresh the checked-in header-only dependencies (see third_party/README.md).
set -euo pipefail
cd "$(dirname "$0")/.."

JSON_VERSION=v3.11.3
CATCH_VERSION=v3.5.4
HTTPLIB_VERSION=v0.15.3
FTXUI_VERSION=v7.0.3

mkdir -p third_party/{nlohmann,catch2,httplib,ftxui}
curl -fsSL -o third_party/nlohmann/json.hpp \
  "https://raw.githubusercontent.com/nlohmann/json/${JSON_VERSION}/single_include/nlohmann/json.hpp"
curl -fsSL -o third_party/catch2/catch_amalgamated.hpp \
  "https://raw.githubusercontent.com/catchorg/Catch2/${CATCH_VERSION}/extras/catch_amalgamated.hpp"
curl -fsSL -o third_party/catch2/catch_amalgamated.cpp \
  "https://raw.githubusercontent.com/catchorg/Catch2/${CATCH_VERSION}/extras/catch_amalgamated.cpp"
curl -fsSL -o third_party/httplib/httplib.h \
  "https://raw.githubusercontent.com/yhirose/cpp-httplib/${HTTPLIB_VERSION}/httplib.h"

tmp_ftxui="$(mktemp)"
trap 'rm -f "$tmp_ftxui"' EXIT
curl -fsSL -o "$tmp_ftxui" \
  "https://github.com/ArthurSonzogni/FTXUI/releases/download/${FTXUI_VERSION}/ftxui-amalgamated.zip"
unzip -jo "$tmp_ftxui" -d third_party/ftxui

echo "vendored json ${JSON_VERSION}, catch2 ${CATCH_VERSION}, cpp-httplib ${HTTPLIB_VERSION}, ftxui ${FTXUI_VERSION}"
