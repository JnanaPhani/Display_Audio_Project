# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Load project secrets/config from a single project-root .env (shared with
# release.py) and inject Supabase URL/key as compile definitions.
include(${CMAKE_CURRENT_LIST_DIR}/load_env.cmake)

# Derive the firmware version from git tags on every build.
# `git describe` yields e.g. "v1.0.0" on a tagged commit, or
# "v1.0.0-3-gabc1234" / "...-dirty" for work past a tag. This becomes the
# app descriptor version (esp_app_get_description()->version), which the
# firmware reads via FIRMWARE_VERSION for logging, heartbeat and the OTA check.
execute_process(
    COMMAND git describe --tags --always --dirty
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    OUTPUT_VARIABLE STD_FW_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT STD_FW_VERSION)
    set(STD_FW_VERSION "0.0.0-nogit")
endif()
set(PROJECT_VER "${STD_FW_VERSION}")
message(STATUS "Firmware version (git): ${PROJECT_VER}")

# Force CMake to re-run (and thus re-derive the version) whenever the git state
# changes: a new commit updates .git/HEAD / the branch ref, and a new tag
# updates packed-refs or refs/tags. This means a plain `idf.py build` after
# `git tag` always picks up the new version with no manual `reconfigure`.
set(GIT_DIR "${CMAKE_CURRENT_LIST_DIR}/.git")
if(EXISTS "${GIT_DIR}/HEAD")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${GIT_DIR}/HEAD")
endif()
if(EXISTS "${GIT_DIR}/packed-refs")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${GIT_DIR}/packed-refs")
endif()
# Track the current branch ref file (updated on each commit).
execute_process(
    COMMAND git rev-parse --abbrev-ref HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    OUTPUT_VARIABLE GIT_BRANCH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(GIT_BRANCH AND EXISTS "${GIT_DIR}/refs/heads/${GIT_BRANCH}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${GIT_DIR}/refs/heads/${GIT_BRANCH}")
endif()

project(smart_token_display)
