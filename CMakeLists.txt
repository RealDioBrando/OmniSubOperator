# Copyright (c) 2026 OmniSubOperator contributors.
#
# Count Phase 0 scaffold. The library is intentionally dependency-free so it
# can be built and tested before either engine integration is attempted.

cmake_minimum_required(VERSION 3.14)

project(
    omni_suboperator
    VERSION 0.1.0
    DESCRIPTION "Shared micro-operators for native execution engines"
    LANGUAGES CXX)

option(OMNI_SUBOP_BUILD_SHARED "Build libomni_subop as a shared library" ON)
option(OMNI_SUBOP_BUILD_TESTS "Build standalone micro-operator tests" ON)

if(OMNI_SUBOP_BUILD_SHARED)
    set(OMNI_SUBOP_LIBRARY_TYPE SHARED)
else()
    set(OMNI_SUBOP_LIBRARY_TYPE STATIC)
endif()

option(OMNI_SUBOP_BUILD_BENCHMARKS "Build standalone micro-operator benchmarks" OFF)

option(OMNI_SUBOP_ENABLE_SVE "Compile the optional ARM SVE walker for the host CPU" OFF)

file(GLOB OMNI_SUBOP_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/kernel/*.cpp)

add_library(omni_subop ${OMNI_SUBOP_LIBRARY_TYPE} ${OMNI_SUBOP_SOURCES})
add_library(omni_subop::omni_subop ALIAS omni_subop)

target_compile_features(omni_subop PUBLIC cxx_std_17)
if(OMNI_SUBOP_ENABLE_SVE)
    if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
        message(WARNING "OMNI_SUBOP_ENABLE_SVE is only supported on aarch64/arm64")
    else()
        include(CheckCXXCompilerFlag)
        check_cxx_compiler_flag("-march=native" OMNI_SUBOP_COMPILER_SUPPORTS_MARCH_NATIVE)
        if(OMNI_SUBOP_COMPILER_SUPPORTS_MARCH_NATIVE)
            target_compile_options(omni_subop PUBLIC -march=native)
        else()
            target_compile_options(omni_subop PUBLIC -march=armv8.2-a+sve)
        endif()
    endif()
endif()
target_include_directories(
    omni_subop
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
           $<INSTALL_INTERFACE:include>)
target_compile_definitions(
    omni_subop PRIVATE OMNI_SUBOP_BUILD=1 OMNI_SUBOP_VERSION_STRING="${PROJECT_VERSION}")

if(NOT OMNI_SUBOP_BUILD_SHARED)
    target_compile_definitions(omni_subop PUBLIC OMNI_SUBOP_STATIC=1)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(omni_subop PRIVATE -Wall -Wextra -Wpedantic)
endif()

set_target_properties(
    omni_subop
    PROPERTIES CXX_VISIBILITY_PRESET hidden
               VISIBILITY_INLINES_HIDDEN YES
               OUTPUT_NAME "omni_subop"
               POSITION_INDEPENDENT_CODE ON)

if(OMNI_SUBOP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(OMNI_SUBOP_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()
