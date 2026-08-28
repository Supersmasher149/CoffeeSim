cmake_minimum_required(VERSION 3.20)
project(espressolab VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(ESPRESSOLAB_BUILD_TESTS  "Build the Catch2 test binary" ON)
option(ESPRESSOLAB_BUILD_SERVER "Build the local REST tool server" ON)
option(ESPRESSOLAB_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

add_library(espressolab_warnings INTERFACE)
if(MSVC)
  target_compile_options(espressolab_warnings INTERFACE /W4)
  if(ESPRESSOLAB_WARNINGS_AS_ERRORS)
    target_compile_options(espressolab_warnings INTERFACE /WX)
  endif()
else()
  target_compile_options(espressolab_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wdouble-promotion)
  if(ESPRESSOLAB_WARNINGS_AS_ERRORS)
    target_compile_options(espressolab_warnings INTERFACE -Werror)
  endif()
endif()

# --- Vendored, header-first third-party dependencies (see third_party/README.md)
add_library(tp_json INTERFACE)
target_include_directories(tp_json SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

add_library(tp_httplib INTERFACE)
target_include_directories(tp_httplib SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/httplib)
find_package(Threads REQUIRED)
target_link_libraries(tp_httplib INTERFACE Threads::Threads)

add_subdirectory(engine/model_library)
add_subdirectory(engine/espresso_core)
add_subdirectory(engine/cfd)
add_subdirectory(engine/cfd3d)
add_subdirectory(engine/artifact_io)
add_subdirectory(engine/reference_io)
add_subdirectory(engine/experiment_runner)
add_subdirectory(engine/calibration)

# Audit F11, issue #16: the separate Level 4b CFD3D solver must never become
# a hidden dependency of the standard Level 1-3 artifact/experiment/
# calibration consumers. Walk each target's transitive LINK_LIBRARIES and
# fail configuration if espressolab_cfd3d shows up anywhere in the closure,
# rather than just checking its direct link list (a future PUBLIC re-leak
# one hop away, the actual shape of this bug, would not show up in a
# direct-only check).
function(espressolab_assert_no_cfd3d_dependency target_name)
  if(NOT TARGET ${target_name})
    message(FATAL_ERROR "espressolab_assert_no_cfd3d_dependency: unknown target ${target_name}")
  endif()
  set(to_visit ${target_name})
  set(visited "")
  while(to_visit)
    list(POP_FRONT to_visit current)
    if(current IN_LIST visited)
      continue()
    endif()
    list(APPEND visited ${current})
    if(current STREQUAL "espressolab_cfd3d")
      message(FATAL_ERROR
        "${target_name} transitively links espressolab_cfd3d -- the separate "
        "Level 4b CFD3D solver must not be a hidden dependency of standard "
        "artifact/experiment/calibration consumers (Audit F11, issue #16).")
    endif()
    if(NOT TARGET ${current})
      continue()
    endif()
    get_target_property(direct_links ${current} LINK_LIBRARIES)
    if(direct_links)
      foreach(link_item ${direct_links})
        # Skip generator expressions ($<...>) and interface library markers;
        # only plain target names matter for this walk.
        if(TARGET ${link_item} AND NOT link_item IN_LIST visited)
          list(APPEND to_visit ${link_item})
        endif()
      endforeach()
    endif()
  endwhile()
endfunction()

espressolab_assert_no_cfd3d_dependency(espressolab_artifacts)
espressolab_assert_no_cfd3d_dependency(espressolab_experiments)
espressolab_assert_no_cfd3d_dependency(espressolab_calibration)

add_subdirectory(apps/espressolab_cli)

if(ESPRESSOLAB_BUILD_SERVER)
  add_subdirectory(apps/espressolab_server)
endif()

if(ESPRESSOLAB_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

message(STATUS "EspressoLab ${PROJECT_VERSION} | build=${CMAKE_BUILD_TYPE} tests=${ESPRESSOLAB_BUILD_TESTS} server=${ESPRESSOLAB_BUILD_SERVER}")
