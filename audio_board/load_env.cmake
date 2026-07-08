# load_env.cmake — portable .env loader for ESP-IDF projects.
#
# Reads KEY=VALUE lines from a project-root .env and:
#   1. exposes each as a CMake variable (ENV_<KEY>), and
#   2. registers SUPABASE_URL / SUPABASE_KEY as global compile definitions so
#      firmware C code can use them without hardcoding secrets in headers.
#
# Real environment variables take precedence over .env entries, so CI can
# override without editing files. Include this BEFORE project() in CMakeLists.

set(_ENV_FILE "${CMAKE_CURRENT_LIST_DIR}/.env")

# Re-run configure if .env changes.
if(EXISTS "${_ENV_FILE}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_ENV_FILE}")

    file(STRINGS "${_ENV_FILE}" _env_lines)
    foreach(_line IN LISTS _env_lines)
        string(STRIP "${_line}" _line)
        # Skip blanks and comments.
        if(_line STREQUAL "" OR _line MATCHES "^#")
            continue()
        endif()
        # Split on the first '='.
        string(FIND "${_line}" "=" _eq)
        if(_eq LESS 0)
            continue()
        endif()
        string(SUBSTRING "${_line}" 0 ${_eq} _key)
        math(EXPR _vstart "${_eq} + 1")
        string(SUBSTRING "${_line}" ${_vstart} -1 _val)
        string(STRIP "${_key}" _key)
        string(STRIP "${_val}" _val)
        # Strip optional surrounding quotes.
        string(REGEX REPLACE "^[\"']" "" _val "${_val}")
        string(REGEX REPLACE "[\"']$" "" _val "${_val}")
        # A real environment variable, if set, wins.
        if(DEFINED ENV{${_key}} AND NOT "$ENV{${_key}}" STREQUAL "")
            set(_val "$ENV{${_key}}")
        endif()
        set("ENV_${_key}" "${_val}")
    endforeach()
else()
    message(WARNING "load_env.cmake: no .env found at ${_ENV_FILE}. "
                    "Copy .env.example to .env (firmware Supabase config will be empty).")
endif()

# Inject Supabase config as compile definitions (firmware reads these).
if(DEFINED ENV_SUPABASE_URL)
    add_compile_definitions("CFG_SUPABASE_URL=\"${ENV_SUPABASE_URL}\"")
endif()
if(DEFINED ENV_SUPABASE_KEY)
    add_compile_definitions("CFG_SUPABASE_ANON_KEY=\"${ENV_SUPABASE_KEY}\"")
endif()

# Inject the ESP-NOW gateway MAC (the 7th "announcer" ESP). Format
# "AA:BB:CC:DD:EE:FF". If unset, espnow.c falls back to broadcast.
if(DEFINED ENV_GATEWAY_MAC)
    add_compile_definitions("CFG_GATEWAY_MAC=\"${ENV_GATEWAY_MAC}\"")
endif()
