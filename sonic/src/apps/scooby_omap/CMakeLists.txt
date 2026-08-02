set(SCOOBY_OMAP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)

option(SCOOBY_SECURE_COMM "Enable secure communication for scooby_omap" ON)

add_library(scooby_omap_core_interface INTERFACE)
target_include_directories(scooby_omap_core_interface
    INTERFACE
        $<BUILD_INTERFACE:${SCOOBY_OMAP_DIR}>
)
target_link_libraries(scooby_omap_core_interface
    INTERFACE
        scooby_node_types
)
if(SCOOBY_SECURE_COMM)
    target_compile_definitions(scooby_omap_core_interface INTERFACE SCOOBY_SECURE_COMM=1)
endif()

add_library(scooby_omap_core_native INTERFACE)
target_link_libraries(scooby_omap_core_native
    INTERFACE
        scooby_omap_core_interface
        sonic::crypto.native
        sonic::omap.native
        sonic::oram.native
)
sonic_target_set_platform(scooby_omap_core_native native)

add_library(scooby_omap_core_sgx INTERFACE)
target_link_libraries(scooby_omap_core_sgx
    INTERFACE
        scooby_omap_core_interface
        sonic::crypto.sgx
        sonic::omap.sgx
        sonic::oram.sgx
)
target_compile_definitions(scooby_omap_core_sgx INTERFACE SONIC_NO_OS=1)
sonic_target_set_platform(scooby_omap_core_sgx sgx)

add_library(scooby_omap_core INTERFACE)
target_link_libraries(scooby_omap_core
    INTERFACE
        $<IF:$<STREQUAL:$<TARGET_PROPERTY:SONIC_PLATFORM>,sgx>,scooby_omap_core_sgx,scooby_omap_core_native>
)

if(SONIC_BUILD_DISTRIBUTED)
    add_library(scooby_omap_runtime_native INTERFACE)
    target_link_libraries(scooby_omap_runtime_native
        INTERFACE
            scooby_omap_core_native
            sonic::sgxbridge.dist
    )
    sonic_target_set_platform(scooby_omap_runtime_native native)

    add_library(scooby_omap_runtime_sgx INTERFACE)
    target_link_libraries(scooby_omap_runtime_sgx
        INTERFACE
            scooby_omap_core_sgx
            sonic::sgxbridge.enclave
    )
    target_compile_definitions(scooby_omap_runtime_sgx INTERFACE SONIC_NO_OS=1)
    sonic_target_set_platform(scooby_omap_runtime_sgx sgx)

    add_library(scooby_omap_runtime INTERFACE)
    target_link_libraries(scooby_omap_runtime
        INTERFACE
            $<IF:$<STREQUAL:$<TARGET_PROPERTY:SONIC_PLATFORM>,sgx>,scooby_omap_runtime_sgx,scooby_omap_runtime_native>
    )
endif()

add_library(scooby_omap INTERFACE)
if(SONIC_BUILD_DISTRIBUTED)
    target_link_libraries(scooby_omap INTERFACE scooby_omap_runtime)
else()
    target_link_libraries(scooby_omap INTERFACE scooby_omap_core)
endif()
