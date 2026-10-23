set(GLUE_DIR ${ANDROID_NDK}/sources/android/native_app_glue)
add_library(diorama64 SHARED main.cpp ${GLUE_DIR}/android_native_app_glue.c)
target_include_directories(diorama64 PRIVATE ${GLUE_DIR})
# sm64_adapter links sm64_game whole (its constructor-registered ROM assets); z: the ROM/asset decompression.
target_link_libraries(diorama64 PRIVATE ar_quest ar_cube_adapter sm64_adapter z)
target_link_options(diorama64 PRIVATE -u ANativeActivity_onCreate)
