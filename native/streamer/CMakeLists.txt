cmake_minimum_required(VERSION 3.20)

project(second_screen_host LANGUAGES CXX)

add_executable(second-screen-host main.cpp)
target_include_directories(second-screen-host PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../shared"
)

target_compile_features(second-screen-host PRIVATE cxx_std_17)
target_compile_definitions(second-screen-host PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00
)

target_link_libraries(second-screen-host PRIVATE
    ws2_32
    ole32
    oleaut32
    mf
    mfplat
    mfuuid
    wmcodecdspuuid
    advapi32
    d3d11
    dxgi
)

# Keep the single-config and Visual Studio layouts consistent with the PRD.
set_target_properties(second-screen-host PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}"
)
