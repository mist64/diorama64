# The Quest settings panel's model, placement and raster: no arvos_* symbol (the app hands the model a Bridge), so the
# host tests link it too.
add_library(quest_panel STATIC panel_model.cpp panel_place.cpp panel_raster.cpp)
target_include_directories(quest_panel PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../third_party)
target_link_libraries(quest_panel PUBLIC ar_core)
