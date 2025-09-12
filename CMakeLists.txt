cmake_minimum_required(VERSION 3.20)
project(udp_scope LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 选项
option(UDP_SCOPE_USE_EPOLLRX "Use Linux recvmmsg/epoll if available" ON)

include(FetchContent)

# GLFW（窗口 + OpenGL 上下文）
FetchContent_Declare(
  glfw
  GIT_REPOSITORY https://github.com/glfw/glfw.git
  GIT_TAG        3.3.8
)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw)

# ImGui
FetchContent_Declare(
  imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG        v1.90.9
)
FetchContent_MakeAvailable(imgui)

# ImPlot（基于 ImGui 的绘图）
FetchContent_Declare(
  implot
  GIT_REPOSITORY https://github.com/epezent/implot.git
  GIT_TAG        v0.16
)
FetchContent_MakeAvailable(implot)

# ===== imgui 静态库（含后端） =====
add_library(imgui_glfw_opengl3 STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui_glfw_opengl3 PUBLIC
  ${imgui_SOURCE_DIR}
  ${imgui_SOURCE_DIR}/backends
)

# ===== implot 静态库 =====
add_library(implot_lib STATIC
  ${implot_SOURCE_DIR}/implot.cpp
  ${implot_SOURCE_DIR}/implot_items.cpp
)
target_include_directories(implot_lib PUBLIC ${implot_SOURCE_DIR})

target_link_libraries(implot_lib PUBLIC imgui_glfw_opengl3)

# ===== 可执行文件 =====
add_executable(udp_scope
  src/main.cpp
)

target_include_directories(udp_scope PRIVATE
  ${imgui_SOURCE_DIR}
  ${imgui_SOURCE_DIR}/backends
  ${implot_SOURCE_DIR}
)

# Linux/Windows 链接项
if (WIN32)
  target_link_libraries(udp_scope PRIVATE implot_lib glfw opengl32)
  target_compile_definitions(udp_scope PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
else()
  find_package(OpenGL REQUIRED)
  target_link_libraries(udp_scope PRIVATE implot_lib glfw OpenGL::GL pthread dl)
endif()

# 更严谨的编译开关
if (MSVC)
  target_compile_options(udp_scope PRIVATE /W4)
else()
  target_compile_options(udp_scope PRIVATE -Wall -Wextra -Wno-unknown-pragmas -Wno-unused-parameter)
endif()