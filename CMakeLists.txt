cmake_minimum_required(VERSION 3.20)
project(memviz LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W3 /permissive- /Zc:__cplusplus /utf-8)
endif()

# --- Dear ImGui (fetched, no CMakeLists of its own) ------------------------
include(FetchContent)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.90.9
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)

# --- memviz executable -----------------------------------------------------
add_executable(memviz WIN32
    src/app.cpp
    src/memscan.cpp
    src/model.cpp
    src/gfx.cpp
)

target_compile_definitions(memviz PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)

target_link_libraries(memviz PRIVATE
    imgui
    d3d11
    dxgi
    d3dcompiler
    psapi
    advapi32
)
