add_library(sonic_omap_interface INTERFACE)
target_include_directories(sonic_omap_interface INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(sonic_omap_interface INTERFACE
    sonic_common_options
)

add_library(sonic_omap_native INTERFACE)
target_link_libraries(sonic_omap_native INTERFACE
    sonic_omap_interface
    sonic::util
    sonic::obliv.native
    sonic::sortshuffle
    sonic::crypto.native
    sonic::threads.native
    sonic::oram.native
)
sonic_target_set_platform(sonic_omap_native native)

add_library(sonic_omap_sgx INTERFACE)
target_link_libraries(sonic_omap_sgx INTERFACE
    sonic_omap_interface
    sonic::util
    sonic::obliv.no_os
    sonic::sortshuffle
    sonic::crypto.sgx
    sonic::threads.no_os
    sonic::oram.sgx
)
target_compile_definitions(sonic_omap_sgx INTERFACE SONIC_NO_OS=1)
sonic_target_set_platform(sonic_omap_sgx sgx)

add_library(sonic_omap INTERFACE)
target_link_libraries(sonic_omap INTERFACE
    $<IF:$<STREQUAL:$<TARGET_PROPERTY:SONIC_PLATFORM>,sgx>,sonic_omap_sgx,sonic_omap_native>
)

add_library(sonic::omap ALIAS sonic_omap)
add_library(sonic::omap.native ALIAS sonic_omap_native)
add_library(sonic::omap.sgx ALIAS sonic_omap_sgx)

add_library(sonic_omap_harness INTERFACE)
target_include_directories(sonic_omap_harness INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(sonic_omap_harness INTERFACE
    sonic::omap
    sonic::util
    sonic::threads
    sonic::crypto
    sonic::oram::harness
)
add_library(sonic::omap::harness ALIAS sonic_omap_harness)

if(SONIC_BUILD_APPLICATIONS)
    add_subdirectory(demo)
endif()
