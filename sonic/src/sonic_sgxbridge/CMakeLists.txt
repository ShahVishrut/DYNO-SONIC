set(SONIC_SGXBRIDGE_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)

set(SONIC_SGXBRIDGE_DISTRIBUTED ${SONIC_BUILD_DISTRIBUTED} CACHE BOOL "enable distributed transport plumbing in sgxbridge" FORCE)

if(NOT TARGET sonic_sgxbridge_common)
    add_library(sonic_sgxbridge_common INTERFACE)
    target_include_directories(sonic_sgxbridge_common
        INTERFACE
            $<BUILD_INTERFACE:${SONIC_SGXBRIDGE_SOURCE_DIR}>
    )
    target_link_libraries(sonic_sgxbridge_common INTERFACE sonic_common_options)
    add_library(sonic::sgxbridge.common ALIAS sonic_sgxbridge_common)
endif()

target_compile_definitions(sonic_sgxbridge_common
    INTERFACE
        SONIC_SGXBRIDGE_DISTRIBUTED=$<BOOL:${SONIC_SGXBRIDGE_DISTRIBUTED}>
)

add_library(sonic_sgxbridge_hostbuf STATIC
    ${SONIC_SGXBRIDGE_SOURCE_DIR}/host/host_buffer_manager.cpp
)
target_include_directories(sonic_sgxbridge_hostbuf
    PUBLIC
        $<BUILD_INTERFACE:${SONIC_SGXBRIDGE_SOURCE_DIR}>
)
target_link_libraries(sonic_sgxbridge_hostbuf
    PUBLIC
        sonic_common_options
)
add_library(sonic::sgxbridge.hostbuf ALIAS sonic_sgxbridge_hostbuf)

if(SONIC_SGXBRIDGE_DISTRIBUTED)
    add_library(sonic_sgxbridge_dist STATIC)
    target_sources(sonic_sgxbridge_dist
        PRIVATE
            ${SONIC_SGXBRIDGE_SOURCE_DIR}/host/dist/api.cpp
    )
    target_include_directories(sonic_sgxbridge_dist
        PUBLIC
            $<BUILD_INTERFACE:${SONIC_SGXBRIDGE_SOURCE_DIR}>
        PRIVATE
            ${CMAKE_SOURCE_DIR}/src
            ${CMAKE_SOURCE_DIR}/src/sonic_util/src
    )
    target_link_libraries(sonic_sgxbridge_dist
        PUBLIC
            sonic_common_options
            sonic::sgxbridge.common
            sonic::sgxbridge.hostbuf
            sonic::dist
    )
else()
    # no code is compiled in core-only builds, keep the target linkable
    add_library(sonic_sgxbridge_dist INTERFACE)
    target_include_directories(sonic_sgxbridge_dist
        INTERFACE
            $<BUILD_INTERFACE:${SONIC_SGXBRIDGE_SOURCE_DIR}>
    )
    target_link_libraries(sonic_sgxbridge_dist
        INTERFACE
            sonic_common_options
            sonic::sgxbridge.common
            sonic::sgxbridge.hostbuf
    )
endif()
add_library(sonic::sgxbridge.dist ALIAS sonic_sgxbridge_dist)

if(NOT SONIC_PLATFORM STREQUAL "sgx")
    return()
endif()

enable_language(C)

set(SONIC_SGXBRIDGE_HOST_SOURCES
    ${SONIC_SGXBRIDGE_SOURCE_DIR}/host/enclave_session.cpp
    ${SONIC_SGXBRIDGE_SOURCE_DIR}/host/storage_manager.cpp
    ${SONIC_SGXBRIDGE_SOURCE_DIR}/host/storage_ocalls.cpp
    ${SONIC_SGXBRIDGE_SOURCE_DIR}/host/filesystem_ocalls.cpp
    ${SONIC_SGXBRIDGE_SOURCE_DIR}/host/hostbuf_ocalls.cpp
    ${SONIC_SGXBRIDGE_SOURCE_DIR}/host/host_services.cpp
    ${SONIC_SGXBRIDGE_SOURCE_DIR}/host/runtime_support.cpp
    ${SONIC_SGXBRIDGE_SOURCE_DIR}/host/dist_ocalls.cpp
    ${SONIC_SGXBRIDGE_SOURCE_DIR}/host/threadpool_manager.cpp
    ${SONIC_SGXBRIDGE_SOURCE_DIR}/host/threadpool_ocalls.cpp
)

sgx_sdk_get_setting(SDK_DIR SONIC_SGXBRIDGE_SDK_DIR)
sgx_sdk_get_setting(LIB_DIR SONIC_SGXBRIDGE_LIB_DIR)

add_library(sonic_sgxbridge_host STATIC ${SONIC_SGXBRIDGE_HOST_SOURCES})
target_include_directories(sonic_sgxbridge_host
    PUBLIC
        $<BUILD_INTERFACE:${SONIC_SGXBRIDGE_SOURCE_DIR}>
    PRIVATE
        ${SONIC_SGXBRIDGE_SDK_DIR}/include
)
target_link_libraries(sonic_sgxbridge_host
    PUBLIC
        sonic_common_options
        sonic::sgxbridge.common
        sonic::sgxbridge.dist
        sonic::sgxbridge.hostbuf
        sonic::storage
        sonic::util
        sonic::threads
)
sonic_target_set_platform(sonic_sgxbridge_host native)
add_library(sonic::sgxbridge.host ALIAS sonic_sgxbridge_host)

set(SONIC_SGXBRIDGE_ENCLAVE_SOURCES
    ${SONIC_SGXBRIDGE_SOURCE_DIR}/enclave/runtime.cpp
    ${SONIC_SGXBRIDGE_SOURCE_DIR}/enclave/host_buffer_portal.cpp
    ${SONIC_SGXBRIDGE_SOURCE_DIR}/enclave/threadpool_portal.cpp
    ${SONIC_SGXBRIDGE_SOURCE_DIR}/enclave/threadpool_provider.cpp
    ${SONIC_SGXBRIDGE_SOURCE_DIR}/enclave/threadpool_registry.cpp
    ${SONIC_SGXBRIDGE_SOURCE_DIR}/enclave/threadpool_worker.cpp
    ${SONIC_SGXBRIDGE_SOURCE_DIR}/enclave/dist_transport.cpp
    ${SONIC_SGXBRIDGE_SOURCE_DIR}/enclave/filesystem_portal.cpp
    ${SONIC_SGXBRIDGE_SOURCE_DIR}/enclave/storage/encrypted_file_backend.cpp
)

if(NOT TARGET sonic_sgxbridge_enclave)
    add_library(sonic_sgxbridge_enclave INTERFACE)
endif()
target_include_directories(sonic_sgxbridge_enclave INTERFACE
    $<BUILD_INTERFACE:${SONIC_SGXBRIDGE_SOURCE_DIR}>
)
target_link_libraries(sonic_sgxbridge_enclave INTERFACE
    sonic_common_options
    sonic::sgxbridge.common
    sonic::storage
    sonic::obliv
    sonic::threads
    sonic::util
    sonic::crypto.sgx
    "${SONIC_SGXBRIDGE_LIB_DIR}/libsgx_pthread.a"
    "${SONIC_SGXBRIDGE_LIB_DIR}/libsgx_tstdc.a"
)
target_compile_definitions(sonic_sgxbridge_enclave INTERFACE
    SONIC_NO_OS=1
    SN_SGX_ENCLAVE=1
)

sgx_sdk_add_trusted_variant(sonic_sgxbridge_enclave
    SOURCES ${SONIC_SGXBRIDGE_ENCLAVE_SOURCES}
)

sonic_target_set_platform(sonic_sgxbridge_enclave sgx)
add_library(sonic::sgxbridge.enclave ALIAS sonic_sgxbridge_enclave)
