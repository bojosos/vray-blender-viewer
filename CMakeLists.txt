cmake_minimum_required(VERSION 3.20)
project(vray_viewport_viewer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static MSVC runtime - no VCRUNTIME DLL dependency on the friend's machine
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# GLFW - fetched and built static (no glfw3.dll shipped with the exe)
include(FetchContent)
set(GLFW_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS       OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS        OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL           OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
)
FetchContent_MakeAvailable(glfw)

add_executable(vray_viewport_viewer main.cpp)

# No Boost needed - shared memory is read via raw Win32 OpenFileMapping/MapViewOfFile
target_link_libraries(vray_viewport_viewer PRIVATE
    glfw
    opengl32
    gdi32
    user32
    shell32
)

if(MSVC)
    set_target_properties(vray_viewport_viewer PROPERTIES LINK_FLAGS "/SUBSYSTEM:CONSOLE")
endif()
