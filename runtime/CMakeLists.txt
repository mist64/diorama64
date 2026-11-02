add_library(ar_capture STATIC capture/fc_builder.cpp)
target_include_directories(ar_capture PUBLIC capture)
target_include_directories(ar_capture PUBLIC include)

add_library(ar_core STATIC core/ar_math.c core/ar_view.c core/ar_float_panel.c core/sarec.c core/snapshot_ring.cpp core/texture_fifo.cpp core/mem_stats.cpp core/runtime.cpp core/audio_ring.cpp core/scene_ring.cpp core/wav_io.cpp core/scene_track.cpp core/audio_settings.cpp core/audio_tuning.c)
target_include_directories(ar_core PUBLIC include)
target_link_libraries(ar_core PUBLIC ar_capture)
if(NOT APPLE)
  find_package(Threads REQUIRED)
  target_link_libraries(ar_core PUBLIC Threads::Threads)
endif()

add_library(ar_render_common STATIC render/common/combiner.cpp render/common/draw_list.cpp render/common/renderer.cpp
            render/common/ar_layer.cpp render/common/debug_points.cpp render/common/scene_shapes.cpp render/common/depth_stats.cpp
            render/common/look.cpp render/common/room_light.cpp
            render/common/room_light_driver.cpp render/common/shadow_fit.cpp render/common/pipeline_warm.cpp)
target_include_directories(ar_render_common PUBLIC render/common)
target_link_libraries(ar_render_common PUBLIC ar_core)

if(APPLE)
add_library(ar_render_metal STATIC render/metal/msl_emitter.cpp render/metal/metal_device.mm)
target_include_directories(ar_render_metal PUBLIC render/metal)
set_source_files_properties(render/metal/metal_device.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
target_link_libraries(ar_render_metal PUBLIC ar_render_common "-framework Metal" "-framework Foundation")

if(NOT SM64AR_VISIONOS)
add_library(ar_png STATIC platform/macos/png_io.mm)
target_include_directories(ar_png PUBLIC platform/macos)
target_compile_options(ar_png PRIVATE -fobjc-arc)
target_link_libraries(ar_png PUBLIC "-framework Foundation" "-framework ImageIO" "-framework CoreGraphics")
endif()
endif()

add_library(ar_platform_fake STATIC platform/fake/fake_platform.c)
target_include_directories(ar_platform_fake PUBLIC platform/fake)
target_link_libraries(ar_platform_fake PUBLIC ar_core)

if(APPLE)
set_source_files_properties(platform/apple/gc_pad.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
add_library(ar_gc_pad STATIC platform/apple/gc_pad.mm)
target_include_directories(ar_gc_pad PUBLIC platform/apple)
target_link_libraries(ar_gc_pad PUBLIC ar_core "-framework GameController" "-framework Foundation")
endif()

add_library(ar_cube_adapter STATIC adapters/cube/cube_adapter.c)
target_include_directories(ar_cube_adapter PUBLIC adapters/cube)
target_link_libraries(ar_cube_adapter PUBLIC ar_core)

if(APPLE AND NOT SM64AR_VISIONOS)
add_library(ar_harness STATIC platform/macos/free_fly.cpp platform/macos/room.cpp
            platform/macos/harness_platform.mm platform/macos/harness_session.mm)
target_include_directories(ar_harness PUBLIC platform/macos)
target_compile_options(ar_harness PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
target_link_libraries(ar_harness PUBLIC ar_render_metal ar_png ar_core ar_gc_pad "-framework Foundation")
endif()

add_library(ar_vos_logic STATIC platform/visionos/vos_log.cpp platform/visionos/vos_options.cpp platform/visionos/vos_placement.cpp
            platform/visionos/vos_state.cpp platform/visionos/vos_resample.cpp platform/visionos/vos_perf.cpp platform/visionos/vos_color.cpp
            platform/visionos/vos_latency.cpp platform/visionos/vos_planes.cpp platform/visionos/vos_input.cpp platform/visionos/vos_place_gesture.cpp
            platform/visionos/vos_scene_audio.cpp platform/visionos/vos_scene_pos.cpp platform/visionos/vos_foveation.cpp
            platform/visionos/vos_safe_mode.cpp platform/visionos/vos_polyphase.cpp)
target_include_directories(ar_vos_logic PUBLIC platform/visionos)
target_link_libraries(ar_vos_logic PUBLIC ar_core ar_render_common)

if(SM64AR_VISIONOS)
  set(AR_VOS_MM platform/visionos/vos_contract.mm platform/visionos/vos_placer.mm platform/visionos/vos_platform.mm
                 platform/visionos/vos_audio.mm platform/visionos/vos_phase.mm)
  set_source_files_properties(${AR_VOS_MM} PROPERTIES COMPILE_OPTIONS -fobjc-arc)
  add_library(ar_vos STATIC ${AR_VOS_MM} platform/visionos/vos_session.cpp platform/visionos/vos_pads.cpp)
  target_include_directories(ar_vos PUBLIC platform/visionos)
  target_link_libraries(ar_vos PUBLIC ar_vos_logic ar_render_metal ar_render_common ar_gc_pad "-framework CompositorServices"
                        "-framework ARKit" "-framework Metal" "-framework Foundation" "-framework AudioToolbox" "-framework AVFAudio"
                        "-framework PHASE")
endif()

if(SM64AR_HOST_TESTS OR SM64AR_QUEST)
  if(NOT ANDROID)
    find_package(Vulkan REQUIRED)
  endif()
  add_library(ar_qx_convert STATIC platform/quest/qx_convert.cpp)
  target_include_directories(ar_qx_convert PUBLIC platform/quest ${SM64AR_OPENXR_DIR}/include)
  target_link_libraries(ar_qx_convert PUBLIC ar_core)
  if(NOT ANDROID)
    target_link_libraries(ar_qx_convert PUBLIC Vulkan::Headers)
  endif()
  # The Quest session's portable logic (host-tested): the lighting it builds from the look, the Touch pad and pointer router,
  # the room-scan table and the placement on it, the spatial mode's panner.
  add_library(ar_qx_logic STATIC platform/quest/qx_lighting.cpp platform/quest/qx_input_pure.cpp platform/quest/qx_scene_pure.cpp
              platform/quest/qx_panner.cpp platform/quest/qx_prefs.cpp)
  target_include_directories(ar_qx_logic PUBLIC platform/quest)
  target_link_libraries(ar_qx_logic PUBLIC ar_render_common ar_core ar_vos_logic ar_qx_convert)

  # glslang compiles the per-combiner GLSL to SPIR-V at runtime: no optimizer (SPIRV-Tools), no HLSL, no tools.
  set(ENABLE_OPT OFF)
  set(ENABLE_GLSLANG_BINARIES OFF)
  set(ENABLE_HLSL OFF)
  set(ENABLE_SPVREMAPPER OFF)
  set(ENABLE_CTEST OFF)
  set(BUILD_EXTERNAL OFF)
  set(GLSLANG_TESTS OFF)
  set(GLSLANG_ENABLE_INSTALL OFF)
  add_subdirectory(${SM64AR_GLSLANG_DIR} ${CMAKE_BINARY_DIR}/glslang EXCLUDE_FROM_ALL)
  add_library(ar_render_vulkan_shaders STATIC render/vulkan/glsl_emitter.cpp render/vulkan/glsl_fixed.cpp)
  target_include_directories(ar_render_vulkan_shaders PUBLIC render/vulkan)
  target_link_libraries(ar_render_vulkan_shaders PUBLIC ar_render_common)
  add_library(ar_glsl_compiler STATIC render/vulkan/glsl_compiler.cpp)
  target_include_directories(ar_glsl_compiler PUBLIC render/vulkan)
  target_link_libraries(ar_glsl_compiler PRIVATE glslang SPIRV glslang-default-resource-limits)
  add_library(ar_render_vulkan STATIC render/vulkan/vulkan_device.cpp render/vulkan/vk_pipelines.cpp render/vulkan/vk_device_pipes.cpp
              render/vulkan/vk_textures.cpp render/vulkan/vk_frame_ring.cpp render/vulkan/vk_resources.cpp render/vulkan/vk_passes.cpp)
  target_include_directories(ar_render_vulkan PUBLIC render/vulkan)
  target_link_libraries(ar_render_vulkan PUBLIC ar_render_common ar_render_vulkan_shaders ar_glsl_compiler PRIVATE ar_vos_logic)
  if(ANDROID)
    target_link_libraries(ar_render_vulkan PUBLIC vulkan)
  else()
    target_link_libraries(ar_render_vulkan PUBLIC Vulkan::Vulkan)
  endif()
endif()

if(SM64AR_QUEST)
  set(DYNAMIC_LOADER ON)
  set(BUILD_API_LAYERS OFF)
  set(BUILD_TESTS OFF)
  set(BUILD_CONFORMANCE_TESTS OFF)
  add_subdirectory(${SM64AR_OPENXR_DIR} ${CMAKE_BINARY_DIR}/openxr)
  add_library(ar_quest STATIC platform/quest/qx_platform.cpp platform/quest/qx_session.cpp platform/quest/qx_input.cpp
              platform/quest/qx_scene.cpp platform/quest/qx_placer.cpp platform/quest/qx_perm.cpp platform/quest/qx_audio.cpp)
  target_include_directories(ar_quest PUBLIC platform/quest PRIVATE ${ANDROID_NDK}/sources/android/native_app_glue)
  target_compile_definitions(ar_quest PUBLIC XR_USE_PLATFORM_ANDROID XR_USE_GRAPHICS_API_VULKAN VK_USE_PLATFORM_ANDROID_KHR)
  target_link_libraries(ar_quest PUBLIC quest_panel ar_qx_convert ar_qx_logic ar_render_vulkan ar_render_common ar_core ar_vos_logic openxr_loader vulkan aaudio android log)
endif()
