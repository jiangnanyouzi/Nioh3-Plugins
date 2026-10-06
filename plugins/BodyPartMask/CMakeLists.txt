get_filename_component(PROJECT_NAME ${CMAKE_CURRENT_SOURCE_DIR} NAME)
project(${PROJECT_NAME})

configure_plugin_project(${PROJECT_NAME})

target_sources(${PROJECT_NAME} PRIVATE
    main.cpp
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/common/include
    ${CMAKE_SOURCE_DIR}/common/include/common
)

target_compile_features(${PROJECT_NAME} PUBLIC cxx_std_23)

# The runtime config lives next to the built DLL (ConfigUtils resolves
# <dll-stem>.ini in the DLL's own directory), so ship the sample beside it.
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/${PROJECT_NAME}.ini"
        "E:/SteamLibrary/steamapps/common/Nioh3/plugins/${PROJECT_NAME}.ini"
    COMMENT "Copying ${PROJECT_NAME}.ini to E:/SteamLibrary/steamapps/common/Nioh3/plugins"
)
