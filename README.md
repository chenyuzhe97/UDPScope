# UDPScope
C++的抓包图形化显示工具


## dependency
sudo apt update
sudo apt install -y build-essential cmake git pkg-config \
  libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev \
  libgl1-mesa-dev mesa-common-dev libglu1-mesa-dev

## deployment
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j
./UdpScopeQt

## 