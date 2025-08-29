cmake_minimum_required(VERSION 3.16)
project(echodemo LANGUAGES CXX)
add_compile_definitions(iperf2)
# 使用 C++23
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 编译警告和调试信息
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(-Wall -Wextra -Wpedantic -g)
endif()

# 公共源文件（Agave 内部实现）
set(AGAVE_SOURCES
    ${CMAKE_SOURCE_DIR}/BJobScheduler.cpp
    # 如果 Agave 还有其他 .cpp 实现文件，这里一并加上
)

include_directories(${CMAKE_SOURCE_DIR}/include)

# ---------- 阻塞版 echo server
add_executable(echo_block echo_block.cpp ${COMMON_SOURCES})
target_link_libraries(echo_block pthread)
# ---------- echo_eventfd ----------
add_executable(echo_eventfd
    eventfd_epoll_echo.cpp
    ${AGAVE_SOURCES}
)
target_link_libraries(echo_eventfd pthread)

# ---------- echo_reuseport ----------
add_executable(echo_reuseport
    so_reuseport_epoll_echo.cpp
    ${AGAVE_SOURCES}
)
target_link_libraries(echo_reuseport pthread)
# ---------- echo_reuseport_lb ----------
add_executable(echo_reuseport_lb
    so_reuseport_lb_epoll_echo.cpp
    ${AGAVE_SOURCES}
)
target_link_libraries(echo_reuseport_lb pthread)