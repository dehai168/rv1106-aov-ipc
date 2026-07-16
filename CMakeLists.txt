cmake_minimum_required(VERSION 3.16)
project(rv1106_aov_ipc C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(ipc_app
  src/main/main.c
  src/common/log.c
  src/common/event_bus.c
)

target_include_directories(ipc_app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_options(ipc_app PRIVATE -Wall -Wextra -Werror)

install(TARGETS ipc_app RUNTIME DESTINATION bin)
