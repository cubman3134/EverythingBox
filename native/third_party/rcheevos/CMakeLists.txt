# Vendored RetroAchievements rcheevos (v12.3.0, MIT). One static lib: the achievement runtime, the
# high-level rc_client, the libretro memory-mapping helper (rc_libretro), and game hashing (rc_hash).
# The RAIntegration / external-client sources are intentionally omitted - their RC_CLIENT_SUPPORTS_*
# macros are left undefined, so rc_client.c compiles without that Windows-only toolkit dependency.
file(GLOB RCHEEVOS_CORE CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/rcheevos/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/rapi/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/rhash/*.c")

add_library(rcheevos STATIC
    ${RCHEEVOS_CORE}
    "${CMAKE_CURRENT_SOURCE_DIR}/src/rc_client.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/rc_compat.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/rc_libretro.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/rc_util.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/rc_version.c")

target_include_directories(rcheevos
    PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/include"
            "${CMAKE_CURRENT_SOURCE_DIR}/src" # rc_libretro.h (the integration header) lives here, not in include/
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/rapi"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/rcheevos"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/rhash"
            "${CMAKE_CURRENT_SOURCE_DIR}/../../src/libretro") # rc_libretro.c needs libretro.h

# Enable the built-in game-hashing API (rc_client_begin_identify_and_load_game) for both the lib and consumers.
target_compile_definitions(rcheevos PUBLIC RC_CLIENT_SUPPORTS_HASH)

if (MSVC)
    target_compile_definitions(rcheevos PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(rcheevos PRIVATE /wd4244 /wd4267) # benign size_t/int narrowing in the C lib
endif()
