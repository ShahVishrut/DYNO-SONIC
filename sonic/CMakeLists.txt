cmake_minimum_required(VERSION 3.20)
project(sonic2 VERSION 2.0.0 LANGUAGES CXX)

if(NOT (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang"))
    message(FATAL_ERROR
        "sonic2 supports only Clang. "
        "Configure with CC=clang CXX=clang++. "
        "Detected CMAKE_CXX_COMPILER_ID='${CMAKE_CXX_COMPILER_ID}'."
    )
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(cmake/sonic_platforms.cmake)
include(cmake/sonic_options.cmake)

if(SONIC_PLATFORM STREQUAL "sgx")
    include(cmake/sgxsdk.cmake)
    include(cmake/sonic_sgx_options.cmake)
    add_subdirectory(src/compat/sgxcxx)
endif()

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

foreach(config ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER ${config} config)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${config} ${CMAKE_BINARY_DIR}/lib)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${config} ${CMAKE_BINARY_DIR}/lib)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${config} ${CMAKE_BINARY_DIR}/bin)
endforeach()

find_package(Threads REQUIRED)

find_program(SONIC_MOLD_LINKER mold)
if(SONIC_MOLD_LINKER)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        add_link_options(-fuse-ld=mold)
        message(STATUS "Using mold linker")
    endif()
endif()

add_library(sonic_common_options INTERFACE)
target_compile_features(sonic_common_options INTERFACE cxx_std_20)

if(MSVC)
    target_compile_options(sonic_common_options INTERFACE /W4)
else()
    target_compile_options(sonic_common_options INTERFACE 
        -Wall -Wextra
        -Wno-unused-parameter
        -Wno-unused-variable
        -Wno-unused-function
    )
endif()

if(SONIC_ENABLE_SANITIZERS)
    target_compile_options(sonic_common_options INTERFACE
        ${SONIC_SANITIZER_FLAG}
        ${SONIC_SANITIZER_COMPILE_EXTRAS}
    )
    target_link_options(sonic_common_options INTERFACE ${SONIC_SANITIZER_FLAG})
endif()

add_subdirectory(src/sonic_util)
add_subdirectory(src/sonic_obliv)
add_subdirectory(src/sonic_sortshuffle)  
add_subdirectory(src/sonic_threads)
add_subdirectory(src/sonic_storage)
add_subdirectory(src/sonic_crypto)
add_subdirectory(src/sonic_oram)
add_subdirectory(src/sonic_omap)
add_subdirectory(src/sonic_dist)
add_subdirectory(src/sonic_sgxbridge)

if(SONIC_BUILD_APPLICATIONS)
    add_subdirectory(src/apps/scooby_node/common)
    add_subdirectory(src/apps/scooby_omap)
    add_subdirectory(src/apps/sonic_demo)
endif()

find_program(CLANG_FORMAT_EXECUTABLE NAMES clang-format clang-format-17 clang-format-16)
if(CLANG_FORMAT_EXECUTABLE)
    file(GLOB_RECURSE SONIC_CLANG_FORMAT_FILES
        RELATIVE ${CMAKE_SOURCE_DIR}
        src/*.c
        src/*.cc
        src/*.cpp
        src/*.cxx
        src/*.h
        src/*.hh
        src/*.hpp
        src/*.hxx)

    list(FILTER SONIC_CLANG_FORMAT_FILES EXCLUDE REGEX "^src/third_party/")
    list(FILTER SONIC_CLANG_FORMAT_FILES EXCLUDE REGEX "^src/sonic_util/src/sonic/util/ext/")

    add_custom_target(format
        COMMAND ${CLANG_FORMAT_EXECUTABLE} -i ${SONIC_CLANG_FORMAT_FILES}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Running clang-format on project sources"
        VERBATIM)
else()
    message(STATUS "clang-format not found: format target will be unavailable")
endif()
