# Shared workflow services (issue #25) and pure TUI navigation/form logic
# (issue #24). Neither file includes FTXUI or touches a terminal, so this
# target stays free of the terminal UI dependency and is linkable by
# espressolab_tests for headless coverage (issue #30).
add_library(espressolab_cli_support STATIC
  workflows.cpp
  sweep_batch_runner.cpp
  tui/tui_forms.cpp)
target_include_directories(espressolab_cli_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(espressolab_cli_support
  PUBLIC espressolab_core espressolab_artifacts espressolab_experiments espressolab_calibration
         espressolab_cfd espressolab_cfd3d espressolab_cfd3d_artifacts espressolab_grind espressolab_warnings tp_json
         Threads::Threads)

# FTXUI is linked only by the CLI executable (issue #28): it never enters the
# solver/model-library dependency graph, and espressolab_cli_support above
# does not need it either.
add_library(tp_ftxui STATIC ${PROJECT_SOURCE_DIR}/third_party/ftxui/ftxui.cpp)
target_include_directories(tp_ftxui SYSTEM PUBLIC ${PROJECT_SOURCE_DIR}/third_party/ftxui)
target_link_libraries(tp_ftxui PUBLIC Threads::Threads)

add_executable(espressolab_cli main.cpp tui/tui.cpp)
target_link_libraries(espressolab_cli PRIVATE espressolab_cli_support tp_ftxui)
