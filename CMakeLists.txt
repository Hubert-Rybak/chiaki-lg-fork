cmake_minimum_required(VERSION 3.16)
project(chiaki-webos C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# ── Options ───────────────────────────────────────────────────────────────────

option(WEBOS_BUILD "Cross-compile for webOS TV" OFF)

set(WEBOS_STAGING_DIR "/tmp/webos-staging" CACHE PATH "Cross-compiled deps staging dir")
set(CHIAKI_SOURCE_DIR "${CMAKE_SOURCE_DIR}/../chiaki-ng" CACHE PATH "chiaki-ng source directory")
set(APP_ID "org.homebrew.chiaki.fork" CACHE STRING "webOS application ID")
set(SDL2_BUNDLED_LIBRARY "" CACHE FILEPATH "SDL-webOS runtime library bundled in the IPK")

if(NOT APP_ID MATCHES "^[a-z0-9][a-z0-9.-]+$")
    message(FATAL_ERROR "Invalid webOS APP_ID: ${APP_ID}")
endif()
message(STATUS "webOS application ID: ${APP_ID}")

# ── CMake module path ─────────────────────────────────────────────────────────
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# ── Platform setup ────────────────────────────────────────────────────────────
if(WEBOS_BUILD OR CMAKE_CROSSCOMPILING)
    message(STATUS "webOS cross-compile build")
    # SDL-webOS exposes its compositor/window extensions from SDL_system.h only
    # when this platform macro is set. The SDK compiler does not define it, but
    # ss4s needs those declarations for its webOS 5 SMP backend.
    add_compile_definitions(__WEBOS__=1)
    list(APPEND CMAKE_PREFIX_PATH "${WEBOS_STAGING_DIR}")
    set(PKG_CONFIG_USE_CMAKE_PREFIX_PATH TRUE)
    set(CMAKE_INSTALL_PREFIX "/app" CACHE STRING "" FORCE)
endif()

# ── Find packages ─────────────────────────────────────────────────────────────
find_package(PkgConfig REQUIRED)

# SDL2 — event loop and input only (ss4s handles A/V output)
pkg_check_modules(SDL2 REQUIRED sdl2)

# JSON-C
pkg_check_modules(JSONC REQUIRED json-c)

# OpenSSL (static)
set(OPENSSL_ROOT_DIR "${WEBOS_STAGING_DIR}")
set(OPENSSL_USE_STATIC_LIBS TRUE)
find_package(OpenSSL REQUIRED)

# ── ss4s ──────────────────────────────────────────────────────────────────────
# ss4s is the hardware A/V pipeline abstraction layer.
# Its webOS NDL modules are built as shared libs (ss4s-ndl-webos4.so etc.)
# and loaded at runtime via dlopen — they are NOT linked into our binary.

# Tell ss4s where to emit module .so files
set(SS4S_MODULE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib" CACHE STRING "" FORCE)
set(SS4S_ENABLE_TESTS   OFF CACHE BOOL "" FORCE)
set(SS4S_ENABLE_SAMPLES OFF CACHE BOOL "" FORCE)
set(SS4S_MODULE_BUILD_DUMMY OFF CACHE BOOL "" FORCE)
set(SS4S_COMPILE_CHECK_STRICT OFF CACHE BOOL "" FORCE)

# NDL DirectMedia — header and stub .so live in the webOS SDK sysroot.
# The real library exists on the TV at runtime; we only need the stub to link.
if(CMAKE_CROSSCOMPILING)
    set(NDL_DIRECTMEDIA_FOUND    TRUE  CACHE BOOL   "" FORCE)
    set(NDL_DIRECTMEDIA_INCLUDE_DIRS "${CMAKE_SYSROOT}/usr/include" CACHE STRING "" FORCE)
    set(NDL_DIRECTMEDIA_LIBRARIES    "NDL_directmedia"              CACHE STRING "" FORCE)
endif()

add_subdirectory("${CMAKE_SOURCE_DIR}/third-party/ss4s" "ss4s_build")

# ── chiaki-ng: build just libchiaki ───────────────────────────────────────────
set(CHIAKI_ENABLE_GUI        OFF CACHE BOOL "" FORCE)
set(CHIAKI_ENABLE_CLI        OFF CACHE BOOL "" FORCE)
set(CHIAKI_ENABLE_SWITCH     OFF CACHE BOOL "" FORCE)
set(CHIAKI_ENABLE_STEAMDECK_NATIVE OFF CACHE BOOL "" FORCE)
set(CHIAKI_ENABLE_SETSU      OFF CACHE BOOL "" FORCE)
set(CHIAKI_ENABLE_TESTS      OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF)

add_subdirectory("${CHIAKI_SOURCE_DIR}/third-party/nanopb"
                 "${CMAKE_BINARY_DIR}/nanopb"
                 EXCLUDE_FROM_ALL)

get_property(_nanopb_targets DIRECTORY "${CHIAKI_SOURCE_DIR}/third-party/nanopb"
             PROPERTY BUILDSYSTEM_TARGETS)
message(STATUS "nanopb targets: ${_nanopb_targets}")

if(NOT TARGET Nanopb::nanopb)
    foreach(_nanopb_tgt nanopb libnanopb protobuf-nanopb-static nanopb_static)
        if(TARGET "${_nanopb_tgt}")
            add_library(Nanopb::nanopb ALIAS "${_nanopb_tgt}")
            message(STATUS "Created Nanopb::nanopb alias for '${_nanopb_tgt}'")
            break()
        endif()
    endforeach()
endif()

if(NOT TARGET Nanopb::nanopb)
    add_library(Nanopb::nanopb INTERFACE IMPORTED GLOBAL)
    message(WARNING "nanopb target not found — created stub")
endif()

set(NANOPB_FOUND TRUE)

macro(nanopb_generate_c SRCS HDRS)
    set(${SRCS})
    set(${HDRS})
    foreach(_proto ${ARGN})
        get_filename_component(_proto_name "${_proto}" NAME_WE)
        set(_pb_c "${CMAKE_CURRENT_BINARY_DIR}/${_proto_name}.pb.c")
        set(_pb_h "${CMAKE_CURRENT_BINARY_DIR}/${_proto_name}.pb.h")
        set_source_files_properties("${_pb_c}" "${_pb_h}" PROPERTIES GENERATED TRUE)
        list(APPEND ${SRCS} "${_pb_c}")
        list(APPEND ${HDRS} "${_pb_h}")
        message(STATUS "nanopb_generate_c (stub): ${_proto_name}.pb.c/h pre-generated")
    endforeach()
endmacro()
macro(NANOPB_GENERATE_C SRCS HDRS)
    nanopb_generate_c(${SRCS} ${HDRS} ${ARGN})
endmacro()

function(fix_pkgconfig_target tgt)
    if(TARGET "${tgt}")
        set_property(TARGET "${tgt}" PROPERTY
            INTERFACE_INCLUDE_DIRECTORIES "${WEBOS_STAGING_DIR}/include")
    endif()
endfunction()

add_subdirectory("${CHIAKI_SOURCE_DIR}/lib" "${CMAKE_BINARY_DIR}/chiaki_lib" EXCLUDE_FROM_ALL)

fix_pkgconfig_target(PkgConfig::json-c)
fix_pkgconfig_target(PkgConfig::MINIUPNPC)
fix_pkgconfig_target(PkgConfig::miniupnpc)

# ── Our executable ────────────────────────────────────────────────────────────
add_executable(chiaki-webos
    src/main.c
    src/config.c
    src/config_import.c
    src/video.c
    src/audio.c
    src/input.c
    src/dualsense.c
    src/root_feedback.c
    src/ui.c
    src/stats.c
)

target_compile_definitions(chiaki-webos PRIVATE
    CHIAKI_APP_ID=\"${APP_ID}\"
)

# Embed rpath so the binary finds ss4s module .so files in lib/ next to itself
set_target_properties(chiaki-webos PROPERTIES
    INSTALL_RPATH    "$ORIGIN/lib"
    BUILD_RPATH      "${CMAKE_BINARY_DIR}/lib"
    BUILD_WITH_INSTALL_RPATH TRUE
)

target_include_directories(chiaki-webos PRIVATE
    src/
    "${CHIAKI_SOURCE_DIR}/lib/include"
    "${WEBOS_STAGING_DIR}/include"
    "${CMAKE_SOURCE_DIR}/third-party/ss4s/include"
    ${SDL2_INCLUDE_DIRS}
    ${JSONC_INCLUDE_DIRS}
)

target_link_libraries(chiaki-webos PRIVATE
    chiaki-lib
    ss4s
    ${SDL2_LIBRARIES}
    ${JSONC_LIBRARIES}
    OpenSSL::SSL
    OpenSSL::Crypto
    opus
    m pthread dl atomic
)

# Enable optional NDL queries for stats overlay (buffer-latency estimate)
if(CMAKE_CROSSCOMPILING OR WEBOS_BUILD)
    target_compile_definitions(chiaki-webos PRIVATE HAVE_NDL_DIRECTMEDIA=1)
    if(DEFINED NDL_DIRECTMEDIA_LIBRARIES)
        target_link_libraries(chiaki-webos PRIVATE ${NDL_DIRECTMEDIA_LIBRARIES})
    else()
        target_link_libraries(chiaki-webos PRIVATE NDL_directmedia)
    endif()
endif()

target_link_options(chiaki-webos PRIVATE
    $<$<CONFIG:Release>:-s>
    -Wl,--allow-multiple-definition
)

# ── Install ───────────────────────────────────────────────────────────────────
install(TARGETS chiaki-webos RUNTIME DESTINATION .)
install(FILES
    packaging/appinfo.json
    packaging/icon.png
    packaging/largeIcon.png
    THIRD-PARTY-NOTICES.md
    DESTINATION .
)
install(DIRECTORY packaging/root/
    DESTINATION root
    USE_SOURCE_PERMISSIONS
)
if(SDL2_BUNDLED_LIBRARY)
    if(NOT EXISTS "${SDL2_BUNDLED_LIBRARY}")
        message(FATAL_ERROR "Bundled SDL runtime not found: ${SDL2_BUNDLED_LIBRARY}")
    endif()
    install(FILES "${SDL2_BUNDLED_LIBRARY}"
        DESTINATION lib
        RENAME libSDL2-2.0.so.0
    )
endif()
# Install ss4s module .so files into lib/ alongside the binary
install(DIRECTORY "${CMAKE_BINARY_DIR}/lib/"
    DESTINATION lib
    OPTIONAL
    FILES_MATCHING PATTERN "ss4s-*.so*"
)

# ── IPK packaging via ares-package ────────────────────────────────────────────
set(IPK_STAGING_DIR "${CMAKE_BINARY_DIR}/ipk-staging")

add_custom_target(ipk
    DEPENDS chiaki-webos

    COMMAND ${CMAKE_COMMAND} -E make_directory "${IPK_STAGING_DIR}"
    COMMAND ${CMAKE_COMMAND} --install "${CMAKE_BINARY_DIR}" --prefix "${IPK_STAGING_DIR}"
    COMMAND ares-package "${IPK_STAGING_DIR}" -o "${CMAKE_BINARY_DIR}"

    COMMENT "Building IPK with ares-package"
    VERBATIM
)
