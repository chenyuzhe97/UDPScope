cmake_minimum_required(VERSION 3.20)
project(udp_scope LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Option (kept for future use; currently unused since we capture via libpcap)
option(UDP_SCOPE_USE_EPOLLRX "Use Linux recvmmsg/epoll if available" ON)

include(FetchContent)

# ----------------------------
# GLFW (window + OpenGL context)
# ----------------------------
FetchContent_Declare(
  glfw
  GIT_REPOSITORY https://github.com/glfw/glfw.git
  GIT_TAG        3.3.8
)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw)

# ----------------------------
# ImGui
# ----------------------------
FetchContent_Declare(
  imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG        v1.90.9
)
FetchContent_MakeAvailable(imgui)

# ----------------------------
# ImPlot (plotting for ImGui)
# ----------------------------
FetchContent_Declare(
  implot
  GIT_REPOSITORY https://github.com/epezent/implot.git
  GIT_TAG        v0.16
)
FetchContent_MakeAvailable(implot)

# ----------------------------
# ImGui static lib (with backends)
# ----------------------------
add_library(imgui_glfw_opengl3 STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
# ImGui headers + backend headers
target_include_directories(imgui_glfw_opengl3 PUBLIC
  ${imgui_SOURCE_DIR}
  ${imgui_SOURCE_DIR}/backends
)
# Important: ensure the backend can find <GLFW/glfw3.h>
target_include_directories(imgui_glfw_opengl3 PUBLIC ${glfw_SOURCE_DIR}/include)
target_link_libraries(imgui_glfw_opengl3 PUBLIC glfw)

# ----------------------------
# ImPlot static lib
# ----------------------------
add_library(implot_lib STATIC
  ${implot_SOURCE_DIR}/implot.cpp
  ${implot_SOURCE_DIR}/implot_items.cpp
)
target_include_directories(implot_lib PUBLIC ${implot_SOURCE_DIR})
target_link_libraries(implot_lib PUBLIC imgui_glfw_opengl3)

# ----------------------------
# Executable
# ----------------------------
add_executable(udp_scope
  src/main.cpp
)

target_include_directories(udp_scope PRIVATE
  ${imgui_SOURCE_DIR}
  ${imgui_SOURCE_DIR}/backends
  ${implot_SOURCE_DIR}
)

# ----------------------------
# OpenGL & platform libs
# ----------------------------
if (WIN32)
  # On Windows you may also need wpcap/Packet if using WinPcap/Npcap.
  # This sample targets Linux primarily; adapt here if building on Windows with pcap.
  target_link_libraries(udp_scope PRIVATE implot_lib glfw opengl32)
  target_compile_definitions(udp_scope PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
else()
  find_package(OpenGL REQUIRED)
  target_link_libraries(udp_scope PRIVATE implot_lib glfw OpenGL::GL pthread dl)
endif()

# ----------------------------
# libpcap (Wireshark-like capture)
# ----------------------------
if (WIN32)
  # Optional: WinPcap/Npcap
  # find_library(PCAP_LIBRARY wpcap)
  # find_library(PACKET_LIBRARY Packet)
  # if (PCAP_LIBRARY AND PACKET_LIBRARY)
  #   target_link_libraries(udp_scope PRIVATE ${PCAP_LIBRARY} ${PACKET_LIBRARY})
  # else()
  #   message(WARNING "Npcap/WinPcap not found; pcap capture will not link on Windows.")
  # endif()
else()
  find_library(PCAP_LIBRARY pcap)
  if (PCAP_LIBRARY)
    target_link_libraries(udp_scope PRIVATE ${PCAP_LIBRARY})
  else()
    message(FATAL_ERROR "libpcap not found. Install it via: sudo apt-get install -y libpcap-dev")
  endif()
endif()

# ----------------------------
# Warnings
# ----------------------------
if (MSVC)
  target_compile_options(udp_scope PRIVATE /W4)
else()
  target_compile_options(udp_scope PRIVATE -Wall -Wextra -Wno-unknown-pragmas -Wno-unused-parameter)
endif()
