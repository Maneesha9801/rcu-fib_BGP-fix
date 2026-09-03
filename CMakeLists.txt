cmake_minimum_required(VERSION 3.24)

project(rcufib
    VERSION 0.1.0
    DESCRIPTION "Lock-free RIB/FIB synchronisation with epoch-based reclamation"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(RCUFIB_BUILD_TESTS "Build the test suite" ON)
option(RCUFIB_BUILD_BENCH "Build the benchmarks" ON)
option(RCUFIB_BUILD_TOOLS "Build the command line tool" ON)
option(RCUFIB_WERROR "Treat warnings as errors" OFF)
set(RCUFIB_SANITIZER "" CACHE STRING "One of: address, thread, undefined, address+undefined")

# --------------------------------------------------------------------- warnings
add_library(rcufib_flags INTERFACE)
target_compile_options(rcufib_flags INTERFACE
    -Wall -Wextra -Wpedantic
    -Wshadow -Wconversion -Wsign-conversion
    -Wnon-virtual-dtor -Wold-style-cast -Woverloaded-virtual
    -Wdouble-promotion -Wformat=2
    # -Wnull-dereference is deliberately absent. It is a heuristic that fires
    # inside libstdc++'s own <vector>, from code this project cannot change, and
    # both GCC and Clang use libstdc++ on Linux. A warning nobody can act on is
    # worse than no warning, because it trains people to ignore the build.
    $<$<BOOL:${RCUFIB_WERROR}>:-Werror>)

# ------------------------------------------------------------------- sanitizers
# Sanitizers must reach both compile and link, and the frame pointer has to
# survive or the reports come back without usable stacks.
if(RCUFIB_SANITIZER)
    string(REPLACE "+" "," RCUFIB_SANITIZER_LIST "${RCUFIB_SANITIZER}")
    target_compile_options(rcufib_flags INTERFACE
        -fsanitize=${RCUFIB_SANITIZER_LIST}
        -fno-omit-frame-pointer
        -fno-optimize-sibling-calls)
    target_link_options(rcufib_flags INTERFACE -fsanitize=${RCUFIB_SANITIZER_LIST})
    message(STATUS "rcufib: sanitizer enabled -> ${RCUFIB_SANITIZER_LIST}")
endif()

# ---------------------------------------------------------------------- library
add_library(rcufib
    src/address.cpp
    src/epoch.cpp
    src/synthetic.cpp
    src/mrt.cpp)
add_library(rcufib::rcufib ALIAS rcufib)

target_include_directories(rcufib PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(rcufib PUBLIC rcufib_flags)

find_package(Threads REQUIRED)
target_link_libraries(rcufib PUBLIC Threads::Threads)

# Optional: read gzip-compressed MRT dumps without an external decompress step.
find_package(ZLIB)
if(ZLIB_FOUND)
    target_link_libraries(rcufib PRIVATE ZLIB::ZLIB)
    target_compile_definitions(rcufib PRIVATE RCUFIB_HAVE_ZLIB=1)
    message(STATUS "rcufib: zlib found, gzip MRT dumps supported")
else()
    target_compile_definitions(rcufib PRIVATE RCUFIB_HAVE_ZLIB=0)
    message(STATUS "rcufib: no zlib, gzip MRT dumps need decompressing first")
endif()

# ------------------------------------------------------------------------ tests
if(RCUFIB_BUILD_TESTS)
    include(FetchContent)
    # Prefer an installed GoogleTest so an offline build still works; fall back
    # to fetching it when there is not one.
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.15.2
        FIND_PACKAGE_ARGS NAMES GTest)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    enable_testing()
    include(GoogleTest)

    add_executable(rcufib_tests
        tests/test_address.cpp
        tests/test_trie.cpp
        tests/test_epoch.cpp
        tests/test_fib.cpp
        tests/test_rib.cpp
        tests/test_concurrency.cpp)
    # GoogleTest's headers are not ours to keep warning-clean, and -Werror
    # would make a warning in them a build failure on some compilers.
    foreach(gtest_target gtest gtest_main gmock gmock_main)
        if(TARGET ${gtest_target})
            set_target_properties(${gtest_target} PROPERTIES SYSTEM TRUE)
        endif()
    endforeach()

    target_link_libraries(rcufib_tests PRIVATE rcufib GTest::gtest_main)
    gtest_discover_tests(rcufib_tests
        DISCOVERY_TIMEOUT 120
        PROPERTIES TIMEOUT 300)
endif()

# ------------------------------------------------------------------- benchmarks
if(RCUFIB_BUILD_BENCH)
    add_executable(rcufib_bench
        bench/main.cpp
        bench/workload.cpp)
    target_link_libraries(rcufib_bench PRIVATE rcufib)
    # The benchmark measures synchronisation, so it must be optimised even when
    # the surrounding build is not.
    target_compile_options(rcufib_bench PRIVATE $<$<CONFIG:Debug>:-O2>)
endif()

# ------------------------------------------------------------------------ tools
if(RCUFIB_BUILD_TOOLS)
    add_executable(rcufib_cli tools/cli.cpp)
    set_target_properties(rcufib_cli PROPERTIES OUTPUT_NAME rcufib)
    target_link_libraries(rcufib_cli PRIVATE rcufib)
endif()

# ---------------------------------------------------------------------- install
include(GNUInstallDirs)
install(TARGETS rcufib rcufib_flags
    EXPORT rcufibTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
