# ---------------------------------------------------------------------------
#  Ulti-Sound Music Player for Nintendo 3DS  -  CMake build
#
#  Build (devkitPro must be installed and $DEVKITPRO set):
#     cmake --preset 3ds
#     cmake --build --preset 3ds
#
#  Or with the Docker toolchain image (see scripts/build.sh):
#     scripts/build.sh
#
#  Output: build/ulti-sound-3ds.3dsx (also copied to the project root).
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.20)

# Fall back to the devkitPro 3DS toolchain when none was supplied on the CLI
# or through a preset, so a bare `cmake -S . -B build` still works.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{DEVKITPRO})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{DEVKITPRO}/cmake/3DS.cmake"
        CACHE FILEPATH "devkitPro Nintendo 3DS toolchain")
endif()

if(NOT DEFINED ENV{DEVKITPRO})
    message(FATAL_ERROR
        "DEVKITPRO is not set. Install devkitPro (with the 3ds-dev group) and "
        "restart your shell, or build inside the devkitpro/devkitarm container.")
endif()

project(ulti-sound-3ds
    VERSION     1.0.0
    LANGUAGES   C
    DESCRIPTION "iPod-inspired 3DS homebrew media player")

# ---------------------------------------------------------------------------
#  Homebrew metadata (written into the SMDH shown by the Homebrew Launcher)
# ---------------------------------------------------------------------------
set(APP_TITLE       "Ulti-Sound Music Player")
set(APP_DESCRIPTION "iPod-style 3DS homebrew media player (audio + MJPEG video)")
set(APP_AUTHOR      "Ulti-Sound")
set(APP_ICON        "${CMAKE_SOURCE_DIR}/icon.png")

# ---------------------------------------------------------------------------
#  Sources  (player + vendored faad2 AAC decoder; all header-only libs live
#  under include/, so there are no external dependencies to fetch)
# ---------------------------------------------------------------------------
file(GLOB SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/source/*.c"
    "${CMAKE_SOURCE_DIR}/source/faad/*.c")

# The devkitPro 3DS toolchain appends the ".elf" suffix and injects the ARM11
# arch flags + `-specs=3dsx.specs` automatically, so we only add our own bits.
add_executable(${PROJECT_NAME} ${SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/source"
    "${CMAKE_SOURCE_DIR}/source/faad"
    "$ENV{DEVKITPRO}/libctru/include")

target_compile_definitions(${PROJECT_NAME} PRIVATE
    __3DS__
    PACKAGE_VERSION="2.11.1")   # faad2 expects its package version defined

target_compile_options(${PROJECT_NAME} PRIVATE
    -Wall -O2 -mword-relocations -ffunction-sections -fdata-sections)

# Drop unused functions/data at link time (pairs with -f*-sections above) to
# shrink the .3dsx. Harmless if a section is actually referenced.
target_link_options(${PROJECT_NAME} PRIVATE -Wl,--gc-sections)

target_link_directories(${PROJECT_NAME} PRIVATE "$ENV{DEVKITPRO}/libctru/lib")
target_link_libraries(${PROJECT_NAME} citro2d citro3d ctru m)

# ---------------------------------------------------------------------------
#  Package the ELF into .smdh + .3dsx for the Homebrew Launcher
# ---------------------------------------------------------------------------
find_program(SMDHTOOL NAMES smdhtool HINTS "$ENV{DEVKITPRO}/tools/bin" REQUIRED)
find_program(TDSXTOOL NAMES 3dsxtool HINTS "$ENV{DEVKITPRO}/tools/bin" REQUIRED)

set(SMDH_FILE "${CMAKE_BINARY_DIR}/${PROJECT_NAME}.smdh")
set(TDSX_FILE "${CMAKE_BINARY_DIR}/${PROJECT_NAME}.3dsx")

add_custom_command(
    OUTPUT  "${SMDH_FILE}"
    COMMAND "${SMDHTOOL}" --create "${APP_TITLE}" "${APP_DESCRIPTION}" "${APP_AUTHOR}"
            "${APP_ICON}" "${SMDH_FILE}"
    DEPENDS "${APP_ICON}"
    COMMENT "Generating SMDH metadata"
    VERBATIM)

add_custom_command(
    OUTPUT  "${TDSX_FILE}"
    COMMAND "${TDSXTOOL}" "$<TARGET_FILE:${PROJECT_NAME}>" "${TDSX_FILE}" "--smdh=${SMDH_FILE}"
    DEPENDS ${PROJECT_NAME} "${SMDH_FILE}"
    COMMENT "Building ${PROJECT_NAME}.3dsx"
    VERBATIM)

add_custom_target(${PROJECT_NAME}_3dsx ALL DEPENDS "${TDSX_FILE}")

# Copy the finished artifacts to the project root for easy grabbing/deploying.
add_custom_command(TARGET ${PROJECT_NAME}_3dsx POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy "${TDSX_FILE}" "${CMAKE_SOURCE_DIR}/${PROJECT_NAME}.3dsx"
    COMMAND ${CMAKE_COMMAND} -E copy "${SMDH_FILE}" "${CMAKE_SOURCE_DIR}/${PROJECT_NAME}.smdh"
    COMMENT "Copying .3dsx / .smdh to the project root"
    VERBATIM)
