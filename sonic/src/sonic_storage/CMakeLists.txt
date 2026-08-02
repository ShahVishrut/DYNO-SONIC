add_library(sonic_storage_interface INTERFACE)
target_include_directories(sonic_storage_interface
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(sonic_storage_interface INTERFACE sonic_common_options sonic::util sonic::threads)

add_library(sonic_storage_native INTERFACE)
target_link_libraries(sonic_storage_native INTERFACE sonic_storage_interface)

add_library(sonic_storage_sgx INTERFACE)
target_link_libraries(sonic_storage_sgx INTERFACE sonic_storage_interface)
target_compile_definitions(sonic_storage_sgx INTERFACE SONIC_NO_OS=1)

add_library(sonic_storage INTERFACE)
target_link_libraries(sonic_storage INTERFACE
    $<IF:$<STREQUAL:$<TARGET_PROPERTY:SONIC_PLATFORM>,sgx>,sonic_storage_sgx,sonic_storage_native>
)

add_library(sonic::storage ALIAS sonic_storage)
add_library(sonic::storage.native ALIAS sonic_storage_native)
add_library(sonic::storage.sgx ALIAS sonic_storage_sgx)
