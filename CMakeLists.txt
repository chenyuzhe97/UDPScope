cmake_minimum_required(VERSION 3.16)
project(UdpScopeQt LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets OpenGL OpenGLWidgets)

# libpcap
find_path(PCAP_INCLUDE_DIR NAMES pcap/pcap.h pcap.h)
find_library(PCAP_LIBRARY pcap)
if (NOT PCAP_INCLUDE_DIR OR NOT PCAP_LIBRARY)
  message(FATAL_ERROR "libpcap not found")
endif()
include_directories(${PCAP_INCLUDE_DIR})

qt_add_executable(UdpScopeQt
  src/main.cpp
  src/MainWindow.cpp
  src/PlotWidget.cpp
  src/PcapWorker.cpp
  src/Core.cpp
  include/MainWindow.hpp
  include/PlotWidget.hpp
  include/PcapWorker.hpp
  include/Core.hpp
)

target_include_directories(UdpScopeQt PRIVATE include)

target_link_libraries(UdpScopeQt PRIVATE
  Qt6::Widgets
  Qt6::OpenGL
  Qt6::OpenGLWidgets
  ${PCAP_LIBRARY}
)

