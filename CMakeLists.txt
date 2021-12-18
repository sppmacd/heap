project(heap)
cmake_minimum_required(VERSION 3.13)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_BUILD_TYPE Debug)

add_executable(heap "main.cpp" "heap.cpp")
target_compile_options(heap PUBLIC -fsanitize=undefined,address)
target_link_options(heap PUBLIC -fsanitize=undefined,address)
