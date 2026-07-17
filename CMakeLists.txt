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
  src/common/sha256.c
  src/common/jpeg_writer.c
  src/system/config_service.c
  src/system/storage_service.c
  src/system/network_service.c
  src/system/system_service.c
  src/media/media_service.c
  src/record/record_service.c
  src/detect/detect_service.c
  src/web/web_service.c
  src/web/fmp4_mux.c
  third_party/cJSON/cJSON.c
  third_party/mongoose/mongoose.c
)

target_include_directories(ipc_app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cJSON
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/rkmuxer
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/mongoose
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
  rkmuxer
  pthread
  m
  stdc++
)

install(TARGETS ipc_app RUNTIME DESTINATION bin)
