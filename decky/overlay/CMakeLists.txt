cmake_minimum_required(VERSION 3.20)
project(fusionhud_overlay C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)

# GLFW 3.4, X11 only, linked in statically.
set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_X11 ON CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw
  URL https://github.com/glfw/glfw/releases/download/3.4/glfw-3.4.zip
  URL_HASH SHA256=b5ec004b2712fd08e8861dc271428f048775200a2df719ccf575143ba749a3e9)

# Dear ImGui v1.91.9b (MIT), compiled in with its GLFW + OpenGL 3 backends.
FetchContent_Declare(imgui
  URL https://github.com/ocornut/imgui/archive/refs/tags/v1.91.9b.tar.gz
  URL_HASH SHA256=8e1bbc76c71d74fef2fb85db7e7ca8eba13d6a86623c54992b60162db554ffdb)

FetchContent_MakeAvailable(glfw imgui)

find_package(X11 REQUIRED)

add_executable(fusionhud-overlay
  src/main.cpp
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(fusionhud-overlay PRIVATE ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_compile_options(fusionhud-overlay PRIVATE -Wall -O2)
# The OpenGL functions come from libGL.so.1 loaded at run time (GLFW + ImGui's loader), so no GL link.
target_link_libraries(fusionhud-overlay PRIVATE glfw X11::X11 ${CMAKE_DL_LIBS} pthread -static-libstdc++ -static-libgcc)
