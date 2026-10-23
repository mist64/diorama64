# The Quest app as a desktop OpenXR program (Linux, Monado): the same bridge and prefs glue as apps/quest.
add_executable(diorama64_desktop main.cpp ${CMAKE_SOURCE_DIR}/apps/quest/quest_app.cpp ${CMAKE_SOURCE_DIR}/apps/quest/quest_prefs_glue.cpp)
target_include_directories(diorama64_desktop PRIVATE ${CMAKE_SOURCE_DIR}/apps/quest ${CMAKE_SOURCE_DIR}/apps/visionos)
target_link_libraries(diorama64_desktop PRIVATE ar_quest ar_cube_adapter sm64_adapter z)
