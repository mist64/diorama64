set(GLUE_DIR ${ANDROID_NDK}/sources/android/native_app_glue)
add_library(diorama64 SHARED main.cpp quest_app.cpp quest_prefs_glue.cpp bundled_copy.cpp ${GLUE_DIR}/android_native_app_glue.c)
# apps/visionos for vos_api.h only (the bridge's C interface, implemented here for the Quest).
target_include_directories(diorama64 PRIVATE ${GLUE_DIR} ${CMAKE_SOURCE_DIR}/apps/visionos)
# sm64_adapter links sm64_game whole (its constructor-registered ROM assets); z: the ROM/asset decompression.
target_link_libraries(diorama64 PRIVATE ar_quest ar_cube_adapter sm64_adapter quest_bundled z android)
target_link_options(diorama64 PRIVATE -u ANativeActivity_onCreate)
