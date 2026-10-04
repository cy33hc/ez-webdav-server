cmake_minimum_required(VERSION 3.5)

project(webdav-server)

# Allow selecting the platform via: cmake -DPLATFORM=PS4 ..
# Defaults to PS5 if not provided.
set(PLATFORM "PS5" CACHE STRING "Target platform (PS4 or PS5)")

# Base compilation flags
set(BASE_C_FLAGS "-O3 -Wno-keyword-macro -fPIC -fPIE -Wall -fstack-protector-all")

# Conditional flags and libraries based on target platform
if(PLATFORM STREQUAL "PS4")
    # PS4 (Jaguar architecture)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${BASE_C_FLAGS} -march=btver2")
    
    # Define PS4-specific libraries to link
    set(PLATFORM_LIBS
        tinyxml2
        SceSystemService
        SceSysCore
        kernel
    )
    
    set(PLATFORM_DEF "PLATFORM_PS4")
    set(OUTPUT_NAME "${PROJECT_NAME}-ps4")
    set(STRIP_TOOL "$ENV{PS4_PAYLOAD_SDK}/bin/orbis-strip")
else()
    # PS5 (Zen 2 architecture) - Also catches any fallback if PLATFORM isn't explicitly PS4
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${BASE_C_FLAGS} -march=znver2")
    
    # Define PS5-specific libraries to link
    set(PLATFORM_LIBS
        tinyxml2
        SceNotification
        SceSystemService
        SceSysCore
        kernel_sys
    )
    
    set(PLATFORM_DEF "PLATFORM_PS5")
    set(OUTPUT_NAME "${PROJECT_NAME}-ps5")
    set(STRIP_TOOL "$ENV{PS5_PAYLOAD_SDK}/bin/prospero-strip")
endif()

# CXX flags mirror C flags with standard overrides
set(CMAKE_CXX_FLAGS "${CMAKE_C_FLAGS} -std=c++20 -gdwarf-2 -O0 -pedantic -pedantic-errors")

set(APP_VERSION "1.00")

include_directories(
  source
)

# Keep the internal CMake target name simple
add_executable(${PROJECT_NAME}
  source/http/httplib.cpp
  source/server/webdav_server.cpp
  source/dbglogger.c
  source/main.cpp
)

# Dynamically set the output name and explicitly append the .elf extension
set_target_properties(${PROJECT_NAME} PROPERTIES
    OUTPUT_NAME "${OUTPUT_NAME}"
    SUFFIX ".elf"
)

# Added PLATFORM_DEF so you can use #ifdef PLATFORM_PS5 in your C++ code
target_compile_definitions(${PROJECT_NAME} PRIVATE 
    APP_VERSION=${APP_VERSION} 
    CPPHTTPLIB_THREAD_POOL_COUNT=64 
    CPPHTTPLIB_NO_ACCEPT4
    ${PLATFORM_DEF}
)

target_link_libraries(${PROJECT_NAME}
    ${PLATFORM_LIBS}
)

# Post-build step for stripping the resulting ELF
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${STRIP_TOOL} $<TARGET_FILE:${PROJECT_NAME}>
    COMMENT "Running strip tool (${STRIP_TOOL}) on the compiled ELF..."
)
