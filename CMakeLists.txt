cmake_minimum_required(VERSION 3.20)
project(dui C CXX)
set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)

set(DUI_PLATFORM "desktop" CACHE STRING
    "Target platform: desktop | macos | ios | android")

find_package(Threads REQUIRED)

# --- d-comms protocol library (sibling repo) ---
add_subdirectory(../d-comms ${CMAKE_CURRENT_BINARY_DIR}/d-comms)

# --- Dear ImGui (common sources, no backend yet) ---
include(FetchContent)
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.6)
FetchContent_MakeAvailable(imgui)

add_library(imgui_core STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp)
target_include_directories(imgui_core PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends)

# --- Platform-specific target ---
if(DUI_PLATFORM STREQUAL "desktop")
    FetchContent_Declare(glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG 3.4)
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(glfw)
    find_package(OpenGL REQUIRED)

    target_sources(imgui_core PRIVATE
        ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
    target_link_libraries(imgui_core PUBLIC glfw OpenGL::GL)

    add_executable(dui
        src/app.cpp
        src/main_desktop.cpp)

elseif(DUI_PLATFORM STREQUAL "macos")
    # TODO: Metal backend
    add_executable(dui MACOSX_BUNDLE
        src/app.cpp src/main_macos.mm)

elseif(DUI_PLATFORM STREQUAL "ios")
    # TODO: UIKit + Metal
    add_executable(dui MACOSX_BUNDLE
        src/app.cpp src/main_ios.mm)
    set_target_properties(dui PROPERTIES
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.dui.app"
        XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "14.0")

elseif(DUI_PLATFORM STREQUAL "android")
    # TODO: ANativeActivity + OpenGL ES 3.0
    add_library(dui SHARED
        src/app.cpp src/main_android.cpp)
endif()

target_link_libraries(dui PRIVATE dcomms_core imgui_core Threads::Threads)
target_include_directories(dui PRIVATE src)
