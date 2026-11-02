# Linux host tool for scripts/packs.py: boots the game headlessly and runs DynOS's generators (the macOS harness's
# --generate-tex / --generate-actors without a window).
add_executable(sm64ar_pack_tool main.cpp)
target_link_libraries(sm64ar_pack_tool PRIVATE sm64_adapter ar_core)
target_compile_options(sm64ar_pack_tool PRIVATE -fno-exceptions)
set_target_properties(sm64ar_pack_tool PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/pack_tool)
