cmake_minimum_required(VERSION 3.20)

# Independent downstream consumer of the installed Backpressure Fabric package.
# This project shares no build tree with the fabric: it only calls
# find_package(BackpressureFabric) and links the exported target.
project(BpfabConsumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(BackpressureFabric 1.0 REQUIRED)

add_executable(bpfab_consumer main.cpp)
target_link_libraries(bpfab_consumer PRIVATE BackpressureFabric::backpressure_fabric)

enable_testing()
add_test(NAME bpfab_consumer COMMAND bpfab_consumer)
