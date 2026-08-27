# Vendored dependencies

Checked in so a clean clone builds offline (Definition of Done, section 2.2).

| Library | Version | Files | Used by |
| --- | --- | --- | --- |
| [nlohmann/json](https://github.com/nlohmann/json) | v3.11.3 | `nlohmann/json.hpp` | `espressolab_artifacts` |
| [Catch2](https://github.com/catchorg/Catch2) | v3.5.4 (amalgamated) | `catch2/catch_amalgamated.{hpp,cpp}` | `espressolab_tests` |
| [cpp-httplib](https://github.com/yhirose/cpp-httplib) | v0.15.3 | `httplib/httplib.h` | `espressolab_server` |
| [FTXUI](https://github.com/ArthurSonzogni/FTXUI) | v7.0.3 | `ftxui/{ftxui.hpp,ftxui.cpp,ftxui_all.hpp}` | `espressolab_cli` TUI |

None of these are visible to `espressolab_core` or `espressolab_models`: the
dependency rule in section 3.4 keeps the simulation library free of JSON, HTTP
filesystem, and terminal UI concerns. FTXUI is linked only by the CLI target.

Refresh with `scripts/vendor_deps.sh`.
