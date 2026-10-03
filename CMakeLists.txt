cmake_minimum_required(VERSION 3.21)
project(CityBuilderDX12 LANGUAGES CXX)

add_executable(CityBuilder WIN32
    src/main.cpp
    src/core/Application.cpp
    src/graphics/Renderer.cpp
)

target_compile_features(CityBuilder PRIVATE cxx_std_20)
target_compile_definitions(CityBuilder PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(CityBuilder PRIVATE src)
target_link_libraries(CityBuilder PRIVATE d3d12 dxgi d3dcompiler)

add_custom_command(TARGET CityBuilder POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:CityBuilder>/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/Cube.hlsl"
        "$<TARGET_FILE_DIR:CityBuilder>/shaders/Cube.hlsl"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/Shadow.hlsl"
        "$<TARGET_FILE_DIR:CityBuilder>/shaders/Shadow.hlsl"
)
