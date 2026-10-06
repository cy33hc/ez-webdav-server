cmake_minimum_required(VERSION 3.5)

project(webdav-server)

# Allow selecting the platform via: cmake -DPLATFORM=PS4 ..
#   PS4 - Build a PS4 payload (requires PS4_PAYLOAD_SDK)
#   PS5 - Build a PS5 payload (requires PS5_PAYLOAD_SDK)   [default]
#   WSL - Build a native Linux/WSL binary for local testing (no console SDK)
set(PLATFORM "PS5" CACHE STRING "Target platform (PS4, PS5 or WSL)")

# Debug logging toggle (default OFF). Enable with: cmake -DDEBUG=ON ..
# When ON, defines DEBUG so DBG_LOG()/log_request() emit via dbglogger; when
# OFF the logging (and the work that builds it) is compiled out entirely.
option(DEBUG "Enable debug logging via dbglogger" OFF)

# Base compilation flags shared by every platform
set(BASE_C_FLAGS "-O3 -Wno-keyword-macro -fPIC -fPIE -Wall -fstack-protector-all")

# Conditional flags and libraries based on target platform
if(PLATFORM STREQUAL "PS4")
    # PS4 (Jaguar architecture)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${BASE_C_FLAGS} -march=btver2")

    # Define PS4-specific libraries to link
    set(PLATFORM_LIBS
        microhttpd
        SceSystemService
        SceSysCore
        kernel
    )

    set(PLATFORM_DEF "PLATFORM_PS4")
    set(OUTPUT_NAME "${PROJECT_NAME}-ps4")
    set(STRIP_TOOL "$ENV{PS4_PAYLOAD_SDK}/bin/orbis-strip")
elseif(PLATFORM STREQUAL "WSL")
    # WSL / native Linux build for local development and testing.
    # Uses the host toolchain; no PlayStation SDK or Sce* libraries required.
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${BASE_C_FLAGS} -march=native")

    # Only portable, host-provided libraries are linked here.
    #   microhttpd - embedded HTTP server (install libmicrohttpd-dev)
    #   pthread    - pthread_create used by main.c (console event thread)
    set(PLATFORM_LIBS
        microhttpd
        pthread
    )

    set(PLATFORM_DEF "PLATFORM_WSL")
    set(OUTPUT_NAME "${PROJECT_NAME}-wsl")
    # No strip tool: keep symbols for native debugging.
    set(STRIP_TOOL "")
else()
    # PS5 (Zen 2 architecture) - Also catches any fallback if PLATFORM isn't explicitly PS4
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${BASE_C_FLAGS} -march=znver2")

    # Define PS5-specific libraries to link
    set(PLATFORM_LIBS
        microhttpd
        SceNotification
        SceSystemService
        SceSysCore
        kernel_sys
    )

    set(PLATFORM_DEF "PLATFORM_PS5")
    set(OUTPUT_NAME "${PROJECT_NAME}-ps5")
    set(STRIP_TOOL "$ENV{PS5_PAYLOAD_SDK}/bin/prospero-strip")
endif()

# The project is now pure C. Compile against C11 with debug info.
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=gnu11 -gdwarf-2")

set(APP_VERSION "1.01")

include_directories(
  source
)

# Keep the internal CMake target name simple.
# The server is now plain C on libmicrohttpd; the former cpp-httplib sources
# (httplib.cpp, webdav_server.cpp, main.cpp) are intentionally excluded.
add_executable(${PROJECT_NAME}
  source/server/webdav_server.c
  source/util.c
  source/dbglogger.c
  source/main.c
)

# For native builds the executable has no custom suffix; console payloads use .elf
if(PLATFORM STREQUAL "WSL")
    set_target_properties(${PROJECT_NAME} PROPERTIES
        OUTPUT_NAME "${OUTPUT_NAME}"
    )
else()
    # Dynamically set the output name and explicitly append the .elf extension
    set_target_properties(${PROJECT_NAME} PROPERTIES
        OUTPUT_NAME "${OUTPUT_NAME}"
        SUFFIX ".elf"
    )
endif()

# PLATFORM_DEF lets the C sources branch with #ifdef PLATFORM_PS5 / PLATFORM_WSL
target_compile_definitions(${PROJECT_NAME} PRIVATE
    APP_VERSION=${APP_VERSION}
    ${PLATFORM_DEF}
    $<$<BOOL:${DEBUG}>:DEBUG>
)

target_link_libraries(${PROJECT_NAME}
    ${PLATFORM_LIBS}
)

# Post-build step for stripping the resulting ELF (console payloads only).
# Skipped for native WSL builds where STRIP_TOOL is empty.
if(STRIP_TOOL)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${STRIP_TOOL} $<TARGET_FILE:${PROJECT_NAME}>
        COMMENT "Running strip tool (${STRIP_TOOL}) on the compiled ELF..."
    )
endif()
