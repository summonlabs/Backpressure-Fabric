cmake_minimum_required(VERSION 3.20)

project(BackpressureFabric
        VERSION 1.0.0
        DESCRIPTION "Vendor-neutral C++20 runtime for generation-bound propagation of network resource pressure"
        HOMEPAGE_URL "https://github.com/summonlabs/Backpressure-Fabric"
        LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Global language requirements
# ---------------------------------------------------------------------------
set(BPFAB_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD ${BPFAB_CXX_STANDARD})
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(BPFAB_IS_TOP_LEVEL ON)
else()
  set(BPFAB_IS_TOP_LEVEL OFF)
endif()

option(BPFAB_BUILD_LIBRARY "Build the Backpressure Fabric runtime library" ON)
option(BPFAB_BUILD_TOOLS   "Build command line tools" ON)
option(BPFAB_BUILD_TESTS   "Build the test suite" ${BPFAB_IS_TOP_LEVEL})
option(BPFAB_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(BPFAB_ENABLE_ASAN   "Build with AddressSanitizer" OFF)
option(BPFAB_ENABLE_UBSAN  "Build with UndefinedBehaviorSanitizer (GCC/Clang only)" OFF)
option(BPFAB_INSTALL       "Generate install rules" ON)

if(NOT BPFAB_BUILD_LIBRARY)
  message(FATAL_ERROR "BPFAB_BUILD_LIBRARY=OFF is not supported: Backpressure Fabric is a library distribution.")
endif()

# ---------------------------------------------------------------------------
# Warning profile
# ---------------------------------------------------------------------------
set(BPFAB_WARNING_OPTIONS "")
set(BPFAB_WARNING_DEFINITIONS "")
if(MSVC)
  list(APPEND BPFAB_WARNING_OPTIONS
    /W4 /permissive- /Zc:__cplusplus /Zc:preprocessor /Zc:ternary /utf-8 /EHsc /MP)
  if(BPFAB_WARNINGS_AS_ERRORS)
    list(APPEND BPFAB_WARNING_OPTIONS /WX)
  endif()
  list(APPEND BPFAB_WARNING_DEFINITIONS
    _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
  list(APPEND BPFAB_WARNING_OPTIONS
    -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wold-style-cast
    -Wcast-align -Wunused -Woverloaded-virtual -Wnull-dereference
    -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough)
  if(BPFAB_WARNINGS_AS_ERRORS)
    list(APPEND BPFAB_WARNING_OPTIONS -Werror)
  endif()
endif()

# Applied directly to each first-party target. A helper interface target would
# leak into the exported target set, which would then require it to be exported
# as well.
function(bpfab_apply_warnings target)
  target_compile_options(${target} PRIVATE ${BPFAB_WARNING_OPTIONS})
  target_compile_definitions(${target} PRIVATE ${BPFAB_WARNING_DEFINITIONS})
endfunction()

set(BPFAB_SANITIZER_FLAGS "")
set(BPFAB_SANITIZER_LINK_FLAGS "")
if(BPFAB_ENABLE_ASAN)
  if(MSVC)
    list(APPEND BPFAB_SANITIZER_FLAGS /fsanitize=address)
  else()
    list(APPEND BPFAB_SANITIZER_FLAGS -fsanitize=address -fno-omit-frame-pointer)
    list(APPEND BPFAB_SANITIZER_LINK_FLAGS -fsanitize=address)
  endif()
endif()
if(BPFAB_ENABLE_UBSAN AND NOT MSVC)
  list(APPEND BPFAB_SANITIZER_FLAGS -fsanitize=undefined -fno-omit-frame-pointer)
  list(APPEND BPFAB_SANITIZER_LINK_FLAGS -fsanitize=undefined)
endif()

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------
set(BPFAB_PUBLIC_HEADERS
  include/backpressure/version.hpp
  include/backpressure/core/status.hpp
  include/backpressure/core/checked.hpp
  include/backpressure/core/fixed.hpp
  include/backpressure/core/ids.hpp
  include/backpressure/core/digest.hpp
  include/backpressure/core/byteio.hpp
  include/backpressure/core/rng.hpp
  include/backpressure/core/clock.hpp
  include/backpressure/model/generation.hpp
  include/backpressure/model/observation.hpp
  include/backpressure/model/fence.hpp
  include/backpressure/model/authority.hpp
  include/backpressure/model/resource.hpp
  include/backpressure/model/dependency.hpp
  include/backpressure/model/topology.hpp
  include/backpressure/model/policy.hpp
  include/backpressure/model/signal.hpp
  include/backpressure/engine/explain.hpp
  include/backpressure/engine/engine.hpp
  include/backpressure/store/fsutil.hpp
  include/backpressure/store/journal.hpp
  include/backpressure/store/durable_state.hpp
  include/backpressure/runtime/fabric.hpp
  include/backpressure/ipc/frame.hpp
  include/backpressure/ipc/subprocess.hpp
  include/backpressure/bench/synthetic.hpp
  include/backpressure/backpressure.hpp
)

set(BPFAB_SOURCES
  src/core/byteio.cpp
  src/core/clock.cpp
  src/core/digest.cpp
  src/model/topology.cpp
  src/model/policy.cpp
  src/model/signal.cpp
  src/engine/engine.cpp
  src/engine/explain.cpp
  src/store/journal.cpp
  src/store/durable_state.cpp
  src/runtime/fabric.cpp
  src/ipc/frame.cpp
  src/ipc/subprocess.cpp
  src/bench/synthetic.cpp
)

add_library(backpressure_fabric ${BPFAB_SOURCES} ${BPFAB_PUBLIC_HEADERS})
add_library(BackpressureFabric::backpressure_fabric ALIAS backpressure_fabric)

target_include_directories(backpressure_fabric
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

bpfab_apply_warnings(backpressure_fabric)
target_compile_options(backpressure_fabric PRIVATE ${BPFAB_SANITIZER_FLAGS})
target_link_options(backpressure_fabric PUBLIC ${BPFAB_SANITIZER_LINK_FLAGS})

find_package(Threads REQUIRED)
target_link_libraries(backpressure_fabric PUBLIC Threads::Threads)

set_target_properties(backpressure_fabric PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  POSITION_INDEPENDENT_CODE ON
  DEFINE_SYMBOL BPFAB_BUILDING_LIBRARY)

target_compile_definitions(backpressure_fabric
  PRIVATE BPFAB_BUILDING_LIBRARY
  PUBLIC BPFAB_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
         BPFAB_VERSION_MINOR=${PROJECT_VERSION_MINOR}
         BPFAB_VERSION_PATCH=${PROJECT_VERSION_PATCH})

if(WIN32)
  target_link_libraries(backpressure_fabric PUBLIC ws2_32)
endif()

# ---------------------------------------------------------------------------
# Tools
# ---------------------------------------------------------------------------
if(BPFAB_BUILD_TOOLS)
  add_executable(bpfab src/tools/bpfab_main.cpp)
  target_link_libraries(bpfab PRIVATE backpressure_fabric)
  bpfab_apply_warnings(bpfab)
  target_compile_options(bpfab PRIVATE ${BPFAB_SANITIZER_FLAGS})
  target_link_options(bpfab PRIVATE ${BPFAB_SANITIZER_LINK_FLAGS})
  set_target_properties(bpfab PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(BPFAB_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Install / export
# ---------------------------------------------------------------------------
if(BPFAB_INSTALL)
  include(GNUInstallDirs)
  include(CMakePackageConfigHelpers)

  install(TARGETS backpressure_fabric
          EXPORT BackpressureFabricTargets
          RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
          LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
          ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
          INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

  if(BPFAB_BUILD_TOOLS)
    install(TARGETS bpfab
            EXPORT BackpressureFabricTargets
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  endif()

  install(DIRECTORY include/backpressure
          DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
          FILES_MATCHING PATTERN "*.hpp")

  set(BPFAB_CMAKE_INSTALL_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/BackpressureFabric)

  install(EXPORT BackpressureFabricTargets
          FILE BackpressureFabricTargets.cmake
          NAMESPACE BackpressureFabric::
          DESTINATION ${BPFAB_CMAKE_INSTALL_DIR})

  configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/BackpressureFabricConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/BackpressureFabricConfig.cmake
    INSTALL_DESTINATION ${BPFAB_CMAKE_INSTALL_DIR})

  write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/BackpressureFabricConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

  install(FILES
            ${CMAKE_CURRENT_BINARY_DIR}/BackpressureFabricConfig.cmake
            ${CMAKE_CURRENT_BINARY_DIR}/BackpressureFabricConfigVersion.cmake
          DESTINATION ${BPFAB_CMAKE_INSTALL_DIR})

  install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/BackpressureFabric)
endif()

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
message(STATUS "Backpressure Fabric ${PROJECT_VERSION}")
message(STATUS "  C++ standard .......... C++${BPFAB_CXX_STANDARD}")
message(STATUS "  Build type ............ ${CMAKE_BUILD_TYPE}")
message(STATUS "  Tools ................. ${BPFAB_BUILD_TOOLS}")
message(STATUS "  Tests ................. ${BPFAB_BUILD_TESTS}")
message(STATUS "  Warnings as errors .... ${BPFAB_WARNINGS_AS_ERRORS}")
message(STATUS "  AddressSanitizer ...... ${BPFAB_ENABLE_ASAN}")