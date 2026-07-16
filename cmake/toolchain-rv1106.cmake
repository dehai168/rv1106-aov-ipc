# Cross toolchain for Luckfox Pico / RV1106 Buildroot (uClibc)
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

if(NOT DEFINED ENV{LUCKFOX_SDK})
  set(LUCKFOX_SDK "/home/user/work/luckfox-pico")
else()
  set(LUCKFOX_SDK "$ENV{LUCKFOX_SDK}")
endif()

set(TOOLCHAIN_DIR "${LUCKFOX_SDK}/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf")
set(CMAKE_C_COMPILER   "${TOOLCHAIN_DIR}/bin/arm-rockchip830-linux-uclibcgnueabihf-gcc")
set(CMAKE_CXX_COMPILER "${TOOLCHAIN_DIR}/bin/arm-rockchip830-linux-uclibcgnueabihf-g++")
set(CMAKE_STRIP        "${TOOLCHAIN_DIR}/bin/arm-rockchip830-linux-uclibcgnueabihf-strip")

set(CMAKE_FIND_ROOT_PATH "${TOOLCHAIN_DIR}/arm-rockchip830-linux-uclibcgnueabihf/sysroot")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(CMAKE_C_FLAGS_INIT "-Wall -Wextra -O2")
set(CMAKE_CXX_FLAGS_INIT "-Wall -Wextra -O2")
