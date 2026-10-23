set(SM64_SRC_DIR ${PROJECT_SOURCE_DIR}/build/_src/sm64)
set(SM64_GEN_DIR ${SM64_SRC_DIR}/build/us_pc)
include(${CMAKE_CURRENT_SOURCE_DIR}/sources.cmake)
foreach(gen ${SM64_GEN_DIR}/include/level_headers.h ${SM64_GEN_DIR}/assets/mario_anim_data.c ${SM64_GEN_DIR}/assets/demo_data.c)
  if(NOT EXISTS ${gen})
    message(FATAL_ERROR "Missing generated file ${gen}\nRun ./build.py (or python3 scripts/sm64_oracle.py) first.")
  endif()
endforeach()

add_library(sm64_sdl_stub STATIC sdl_stub/sdl_stub.c)
target_include_directories(sm64_sdl_stub PUBLIC sdl_stub/include)

if(SM64AR_VISIONOS)
  # Not available on visionOS: SDL backends, curl (update checker), the SDL crash handler.
  list(FILTER SM64_C_SOURCES EXCLUDE REGEX "src/pc/(audio/audio_sdl|controller/controller_sdl|update_checker|crash_handler)\\.c$")
  # miniaudio's Core Audio backend includes AVFoundation (Objective-C) on iOS-family targets; mod audio is unused in AR.
  set_source_files_properties(${SM64_SRC_DIR}/src/pc/utils/miniaudio.c PROPERTIES COMPILE_DEFINITIONS MA_NO_COREAUDIO)
  set(SM64_SDL sm64_sdl_stub)
  set(SM64_PLATFORM_DEFINES "")
  set(SM64_PLATFORM_LIBS z "-framework CoreFoundation" "-framework Foundation")
  set(SM64_ADAPTER_EXTRA adapter/src/link_stubs_visionos.c)
  # OSX_BUILD means desktop macOS (SDL2/GL). Dropped so a desktop-only #ifdef OSX_BUILD path can never switch on for
  # visionOS.
  list(REMOVE_ITEM SM64_DEFINES OSX_BUILD=1)
elseif(NOT APPLE)
  # Embedded library off Apple (Linux host: pack tool and tests; Android: the Quest app). Same exclusions as visionOS:
  # no SDL backends, curl, or SDL crash handler; nothing is drawn through SDL/GL. miniaudio keeps its defaults (its
  # backends are opened with dlopen, nothing is linked).
  list(FILTER SM64_C_SOURCES EXCLUDE REGEX "src/pc/(audio/audio_sdl|controller/controller_sdl|update_checker|crash_handler)\\.c$")
  set(SM64_SDL sm64_sdl_stub)
  set(SM64_PLATFORM_DEFINES "")
  if(ANDROID)
    # Bionic's <strings.h> defines bcopy/bzero as macros, which PR/os_libc.h's fallback extern declarations would
    # expand; this is upstream's own switch to its memcpy/memset path.
    set(SM64_PLATFORM_DEFINES NO_BZERO_BCOPY)
    set(SM64_PLATFORM_LIBS z log android)
  else()
    find_package(Threads REQUIRED)
    set(SM64_PLATFORM_LIBS z m dl Threads::Threads)
  endif()
  set(SM64_ADAPTER_EXTRA adapter/src/link_stubs_visionos.c)
  # No CommonCrypto off Apple: sm64_pack_stamp.c hashes through sha256_portable.c.
  set(SM64_ADAPTER_PORTABLE adapter/src/sha256_portable.c)
  # OSX_BUILD means desktop macOS (SDL2/GL); never on here.
  list(REMOVE_ITEM SM64_DEFINES OSX_BUILD=1)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
  set(SM64_SDL PkgConfig::SDL2)
  set(SM64_PLATFORM_DEFINES HAVE_SDL2=1)
  set(SM64_PLATFORM_LIBS z curl "-framework CoreFoundation" "-framework IOKit" "-framework AppKit" "-framework OpenGL")
  set(SM64_ADAPTER_EXTRA "")
endif()

# Lua 5.3.5 from the pinned upstream tarball (scripts/fetch_lua.py). sm64_game compiles against these headers too
# (not upstream's lib/lua/include copy), so luaconf/ABI have one source of truth on both platforms.
set(LUA_SRC_DIR ${PROJECT_SOURCE_DIR}/build/_deps/lua-5.3.5/src)
set(LUA_SRC_DIR ${LUA_SRC_DIR} PARENT_SCOPE)
if(NOT EXISTS ${LUA_SRC_DIR}/lapi.c)
  message(FATAL_ERROR "Missing Lua 5.3.5 sources in ${LUA_SRC_DIR}\nRun python3 scripts/fetch_lua.py first.")
endif()

# Upstream compile state (defines + include dirs). PRIVATE to the two targets that compile upstream
# headers, so consumers of sm64_adapter (tests, runtime, harness) never see VERSION_US, F3DEX_*, etc.
add_library(sm64_upstream_headers INTERFACE)
target_compile_definitions(sm64_upstream_headers INTERFACE ${SM64_DEFINES} _LANGUAGE_C DYNOS SM64_AR_LIBRARY
  ${SM64_PLATFORM_DEFINES} ENHANCE_LEVEL_TEXTURES)
target_include_directories(sm64_upstream_headers INTERFACE
  ${SM64_SRC_DIR}/include ${SM64_GEN_DIR} ${SM64_GEN_DIR}/include ${SM64_SRC_DIR}/src ${SM64_SRC_DIR}
  ${SM64_SRC_DIR}/sound ${LUA_SRC_DIR} ${SM64_SRC_DIR}/lib/coopnet/include)
if(SM64AR_VISIONOS OR NOT APPLE)
  target_include_directories(sm64_upstream_headers INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/sdl_stub/include)
endif()

set(LUA_NAMES lapi lcode lctype ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes lparser lstate lstring ltable
    ltm lundump lvm lzio lauxlib lbaselib lbitlib lcorolib ldblib liolib lmathlib loslib lstrlib ltablib lutf8lib
    loadlib linit)
list(TRANSFORM LUA_NAMES PREPEND ${LUA_SRC_DIR}/ OUTPUT_VARIABLE LUA_SOURCES)
list(TRANSFORM LUA_SOURCES APPEND .c)
add_library(sm64_lua STATIC ${LUA_SOURCES} lua/lua_no_system.c)
target_include_directories(sm64_lua PRIVATE ${LUA_SRC_DIR})
# LUA_COMPAT_5_2: what Lua 5.3's own makefile builds with (mods may use 5.2 compatibility functions).
target_compile_definitions(sm64_lua PRIVATE LUA_USE_POSIX LUA_COMPAT_5_2)
target_compile_options(sm64_lua PRIVATE -O2 -w)
set_source_files_properties(${LUA_SRC_DIR}/loslib.c PROPERTIES COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/lua/lua_no_system.h")

# The stubs stand in for upstream modules; only sm64_game references them. GNU ld (Linux host) scans an archive once, in
# link order, so off Apple they are built into sm64_game (linked whole) instead of the sm64_adapter archive before it.
# On Apple the adapter's sources stay exactly as they were.
if(APPLE)
  set(SM64_GAME_STUBS "")
  set(SM64_ADAPTER_STUBS adapter/src/link_stubs.c)
  set(SM64_ADAPTER_TAIL ${SM64_ADAPTER_EXTRA})
else()
  set(SM64_GAME_STUBS adapter/src/link_stubs.c ${SM64_ADAPTER_EXTRA})
  set(SM64_ADAPTER_STUBS "")
  set(SM64_ADAPTER_TAIL ${SM64_ADAPTER_PORTABLE})
endif()
add_library(sm64_game STATIC ${SM64_C_SOURCES} ${SM64_CXX_SOURCES} ${SM64_GAME_STUBS})
target_compile_options(sm64_game PRIVATE -fno-strict-aliasing -fwrapv -fsigned-char -O2 -w)
# PRIVATE on a static library still propagates the link dependencies ($<LINK_ONLY:...>), not the compile state.
target_link_libraries(sm64_game PRIVATE sm64_upstream_headers ${SM64_SDL} sm64_lua ${SM64_PLATFORM_LIBS})

add_library(sm64_audio_logic STATIC adapter/src/sfx_lanes.c adapter/src/sm64_relevance.c adapter/src/sm64_orchestra.c
            adapter/src/sm64_event_music.c)
target_include_directories(sm64_audio_logic PUBLIC adapter/include)
target_link_libraries(sm64_audio_logic PUBLIC ar_core)
add_library(sm64_adapter STATIC adapter/src/sm64_host.c ${SM64_ADAPTER_STUBS} adapter/src/ar_network_null.c adapter/src/ground_height.c adapter/src/ar_controller.c adapter/src/pad_map.c adapter/src/gfx_capture_api.c adapter/src/shading_view.c adapter/src/ar_adapter_sm64.c adapter/src/ar_setup.c adapter/src/sm64_ar_view.c adapter/src/ar_game_hooks.c adapter/src/ar_audio.c adapter/src/ar_hud.c adapter/src/ar_hud_logic.c adapter/src/ar_pip.c adapter/src/sm64_pip.c adapter/src/ar_cannon.c adapter/src/ar_cannon_frame.c adapter/src/ar_trees.c adapter/src/ar_loop.c adapter/src/sm64_levels.c adapter/src/sm64_vis.c adapter/src/sm64_light.c adapter/src/sm64_packs.c adapter/src/sm64_story.c adapter/src/sm64_touch.c adapter/src/sm64_eyes.c adapter/src/sm64_settings.c adapter/src/sm64_look.c adapter/src/ar_look.c adapter/src/sm64_rooms.c adapter/src/sm64_pack_stamp.c ${SM64_ADAPTER_TAIL})
target_include_directories(sm64_adapter PUBLIC adapter/include)
target_compile_options(sm64_adapter PRIVATE -fsigned-char)
# sm64_game has __attribute__((constructor)) ROM-asset registrations in objects nothing references;
# a plain static link would drop them (no music/textures), so pull in the whole archive.
target_link_libraries(sm64_adapter PRIVATE sm64_upstream_headers ${SM64_SDL} PUBLIC "$<LINK_LIBRARY:WHOLE_ARCHIVE,sm64_game>")
target_link_libraries(sm64_adapter PUBLIC ar_core sm64_audio_logic)
if(SM64AR_TESTS AND NOT SM64AR_VISIONOS)
  target_compile_definitions(sm64_adapter PRIVATE SM64AR_TEST_SEAMS=1)
endif()

