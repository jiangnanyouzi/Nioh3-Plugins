get_filename_component(PROJECT_NAME ${CMAKE_CURRENT_SOURCE_DIR} NAME)
project(${PROJECT_NAME})

configure_plugin_project(${PROJECT_NAME})

target_sources(${PROJECT_NAME} PRIVATE
    main.cpp
    src/config.cpp
    src/discover.cpp
    src/purple.cpp
    src/maps.cpp
    src/pair.cpp
    src/roster.cpp
)

# Headers live in src/: main.cpp includes "src/patterns.h", the modules include
# "core.h" / "state.h" / "config.h".
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# HookStub.asm is assembled outside the VS MASM rule: with
# CMAKE_CXX_EXTENSIONS OFF CMake leaks /Zc:__cplusplus into the <MASM> item
# definition and ml64 then produces no .obj (LNK1181). A custom command with
# a clean ml64 line sidesteps the leak entirely; the resulting .obj is
# linked like any object file.
get_filename_component(_vctools "${CMAKE_CXX_COMPILER}" DIRECTORY)
set(_ml64 "${_vctools}/ml64.exe")
if(NOT EXISTS "${_ml64}")
    message(FATAL_ERROR "ml64.exe not found next to cl.exe: ${_ml64}")
endif()
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/HookStub.obj
    COMMAND "${_ml64}" -nologo -c
            -Fo${CMAKE_CURRENT_BINARY_DIR}/HookStub.obj
            ${CMAKE_CURRENT_SOURCE_DIR}/HookStub.asm
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/HookStub.asm
    COMMENT "Assembling HookStub.asm (custom, flag-clean)"
)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/HookStub.obj)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/common/include
    ${CMAKE_SOURCE_DIR}/common/include/common
)

target_compile_features(${PROJECT_NAME} PUBLIC cxx_std_23)
