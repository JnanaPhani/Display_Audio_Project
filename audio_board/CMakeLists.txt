# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Load project secrets/config from a single project-root .env and inject Supabase URL/key as compile definitions.
include(${CMAKE_CURRENT_LIST_DIR}/load_env.cmake)

# Derive the firmware version from git tags on every build.
execute_process(
    COMMAND git describe --tags --always --dirty
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    OUTPUT_VARIABLE AB_FW_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT AB_FW_VERSION)
    set(AB_FW_VERSION "0.0.0-nogit")
endif()
set(PROJECT_VER "${AB_FW_VERSION}")
message(STATUS "Firmware version (git): ${PROJECT_VER}")

# Force CMake to re-run whenever the git state changes.
set(GIT_DIR "${CMAKE_CURRENT_LIST_DIR}/.git")
if(EXISTS "${GIT_DIR}/HEAD")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${GIT_DIR}/HEAD")
endif()
if(EXISTS "${GIT_DIR}/packed-refs")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${GIT_DIR}/packed-refs")
endif()
execute_process(
    COMMAND git rev-parse --abbrev-ref HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    OUTPUT_VARIABLE GIT_BRANCH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(GIT_BRANCH AND EXISTS "${GIT_DIR}/refs/heads/${GIT_BRANCH}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${GIT_DIR}/refs/heads/${GIT_BRANCH}")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(audio_board)
