cmake_minimum_required(VERSION 3.16)
project(rv1106_aov_ipc C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED ENV{LUCKFOX_SDK})
  set(LUCKFOX_SDK "/home/user/work/luckfox-pico")
else()
  set(LUCKFOX_SDK "$ENV{LUCKFOX_SDK}")
endif()

set(RK_MEDIA_OUT "${LUCKFOX_SDK}/media/out")

add_executable(ipc_app
  src/main/main.c
  src/common/log.c
  src/common/event_bus.c
  src/system/config_service.c
  src/media/media_service.c
  third_party/cJSON/cJSON.c
)

target_include_directories(ipc_app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cJSON
  ${RK_MEDIA_OUT}/include
  ${RK_MEDIA_OUT}/include/rkaiq
  ${RK_MEDIA_OUT}/include/rkaiq/uAPI2
  ${RK_MEDIA_OUT}/include/rkaiq/common
  ${RK_MEDIA_OUT}/include/rkaiq/xcore
  ${RK_MEDIA_OUT}/include/rkaiq/algos
  ${RK_MEDIA_OUT}/include/rkaiq/iq_parser
  ${RK_MEDIA_OUT}/include/rkaiq/iq_parser_v2
  ${RK_MEDIA_OUT}/include/rkaiq/smartIr
)

target_compile_options(ipc_app PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(ipc_app PRIVATE
  _GNU_SOURCE
  RKAIQ
  UAPI2
  RV1106_RV1103
)

target_link_directories(ipc_app PRIVATE ${RK_MEDIA_OUT}/lib)
target_link_options(ipc_app PRIVATE
  "-Wl,-rpath-link,${RK_MEDIA_OUT}/lib"
)
target_link_libraries(ipc_app PRIVATE
  rockit
  rockchip_mpp
  rga
  rkaiq
  pthread
  m
  stdc++
)

install(TARGETS ipc_app RUNTIME DESTINATION bin)
