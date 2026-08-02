add_library(sonic_oram_interface INTERFACE)

target_include_directories(sonic_oram_interface INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(sonic_oram_interface INTERFACE
    sonic_common_options
    sonic::storage
)

if(SONIC_ORAM_TIERED_STORAGE)
    target_compile_definitions(sonic_oram_interface INTERFACE SONIC_ORAM_TIERED_STORAGE=1)
endif()
if(SONIC_ORAM_METRICS)
    target_compile_definitions(sonic_oram_interface INTERFACE SONIC_ORAM_METRICS=1)
endif()

if(SONIC_ORAM_DEBUG)
    target_compile_definitions(sonic_oram_interface INTERFACE ORAM_DEBUG)
endif()

add_library(sonic_oram_native INTERFACE)
target_link_libraries(sonic_oram_native INTERFACE
    sonic_oram_interface
    sonic::util
    sonic::obliv.native
    sonic::sortshuffle
    sonic::crypto.native
    sonic::threads.native
)
sonic_target_set_platform(sonic_oram_native native)

add_library(sonic_oram_sgx INTERFACE)
target_link_libraries(sonic_oram_sgx INTERFACE
    sonic_oram_interface
    sonic::util
    sonic::obliv.no_os
    sonic::sortshuffle
    sonic::crypto.sgx
    sonic::threads.no_os
)
target_compile_definitions(sonic_oram_sgx INTERFACE SONIC_NO_OS=1)
sonic_target_set_platform(sonic_oram_sgx sgx)

add_library(sonic_oram INTERFACE)
target_link_libraries(sonic_oram INTERFACE
    $<IF:$<STREQUAL:$<TARGET_PROPERTY:SONIC_PLATFORM>,sgx>,sonic_oram_sgx,sonic_oram_native>
)

add_library(sonic::oram ALIAS sonic_oram)
add_library(sonic::oram.native ALIAS sonic_oram_native)
add_library(sonic::oram.sgx ALIAS sonic_oram_sgx)

add_library(sonic_oram_harness INTERFACE)
target_include_directories(sonic_oram_harness INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(sonic_oram_harness INTERFACE
    sonic::oram
    sonic::util
    sonic::threads
    sonic::crypto
)
add_library(sonic::oram::harness ALIAS sonic_oram_harness)

if(SONIC_BUILD_APPLICATIONS)
    add_subdirectory(demo)
endif()
