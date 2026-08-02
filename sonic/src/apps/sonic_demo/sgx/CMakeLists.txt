if(NOT SONIC_PLATFORM STREQUAL "sgx")
    return()
endif()

enable_language(C)

set(SONIC_DEMO_SGX_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(SONIC_DEMO_SGX_HOST_DIR ${SONIC_DEMO_SGX_DIR}/host)
set(SONIC_DEMO_SGX_ENCLAVE_DIR ${SONIC_DEMO_SGX_DIR}/enclave)
set(SONIC_DEMO_SGX_EDL ${SONIC_DEMO_SGX_DIR}/sonic_demo.edl)

sgx_sdk_get_setting(SDK_DIR SONIC_DEMO_SGX_SDK_DIR)
sgx_sdk_get_setting(LIB_DIR SONIC_DEMO_SGX_LIB_DIR)

sgx_sdk_generate_edl(
    NAME sonic_demo
    EDL "${SONIC_DEMO_SGX_EDL}"
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated"
    SEARCH_PATHS "${CMAKE_SOURCE_DIR}/src/sonic_sgxbridge/edl"
    OUT_TRUSTED_C SONIC_DEMO_SGX_TRUSTED_C
    OUT_TRUSTED_H SONIC_DEMO_SGX_TRUSTED_H
    OUT_UNTRUSTED_C SONIC_DEMO_SGX_UNTRUSTED_C
    OUT_UNTRUSTED_H SONIC_DEMO_SGX_UNTRUSTED_H
    OUT_DIR_VAR SONIC_DEMO_SGX_GENERATED_DIR
    OUT_TARGET SONIC_DEMO_SGX_EDL_TARGET
)

set(SONIC_DEMO_SGX_ENCLAVE_SOURCES
    ${SONIC_DEMO_SGX_ENCLAVE_DIR}/main.cpp
    ${SONIC_DEMO_SGX_TRUSTED_C}
)

set(SONIC_DEMO_SGX_ENCLAVE_HEADERS
    ${SONIC_DEMO_SGX_TRUSTED_H}
)

sgx_sdk_add_enclave(sonic_demo_sgx_enclave
    SOURCES ${SONIC_DEMO_SGX_ENCLAVE_SOURCES}
    HEADERS ${SONIC_DEMO_SGX_ENCLAVE_HEADERS}
    INCLUDE_DIRS
        ${SONIC_DEMO_SGX_GENERATED_DIR}
        ${SONIC_DEMO_SGX_ENCLAVE_DIR}
        ${SONIC_DEMO_LOGIC_DIR}
        ${SONIC_DEMO_COMMON_DIR}
    EDL_TARGET "${SONIC_DEMO_SGX_EDL_TARGET}"
    CONFIG_TOML "${SONIC_DEMO_SGX_ENCLAVE_DIR}/enclave.config.toml"
    VERSION_SCRIPT "${SONIC_DEMO_SGX_ENCLAVE_DIR}/enclave.lds"
    OUTPUT_NAME "sonic_demo_enclave"
    SIGNED_OUTPUT_VAR SONIC_DEMO_SGX_SIGNED_ENCLAVE
    LINK_LIBS
        "${SONIC_DEMO_SGX_LIB_DIR}/libsgx_pthread.a"
        sonic_demo_logic
        sonic::oram::harness
        sonic::omap::harness
        sonic::sgxbridge.enclave
        sonic::util
        sonic::threads
)
sonic_target_set_platform(sonic_demo_sgx_enclave sgx)
target_compile_definitions(sonic_demo_sgx_enclave
    PRIVATE
        SN_SGX_ENCLAVE=1
)

set(SONIC_DEMO_SGX_HOST_SOURCES
    ${SONIC_DEMO_SGX_HOST_DIR}/main.cpp
    ${SONIC_DEMO_SGX_UNTRUSTED_C}
)

sgx_sdk_add_host(sonic_demo_sgx_host
    SOURCES ${SONIC_DEMO_SGX_HOST_SOURCES}
    HEADERS ${SONIC_DEMO_SGX_UNTRUSTED_H}
    INCLUDE_DIRS
        ${SONIC_DEMO_SGX_GENERATED_DIR}
        ${SONIC_DEMO_CLI_DIR}
        ${SONIC_DEMO_COMMON_DIR}
    LINK_LIBS
        sonic_demo_cli
        sonic_demo_common
        sonic::sgxbridge.host
        sonic::util
        sonic::threads
    EDL_TARGET "${SONIC_DEMO_SGX_EDL_TARGET}"
    SIGNED_ENCLAVE "${SONIC_DEMO_SGX_SIGNED_ENCLAVE}"
    COPY_SIGNED
)
sonic_target_set_platform(sonic_demo_sgx_host native)

sgx_sdk_get_sign_target(sonic_demo_sgx_enclave SONIC_DEMO_SGX_SIGN_TARGET)
sgx_sdk_get_runtime_signed_target(sonic_demo_sgx_enclave SONIC_DEMO_SGX_RUNTIME_SIGN_TARGET)
if(SONIC_DEMO_SGX_RUNTIME_SIGN_TARGET)
    add_dependencies(sonic_demo_sgx_host "${SONIC_DEMO_SGX_RUNTIME_SIGN_TARGET}")
else()
    add_dependencies(sonic_demo_sgx_host "${SONIC_DEMO_SGX_SIGN_TARGET}")
endif()

sgx_sdk_get_setting(MODE SONIC_DEMO_SGX_ACTIVE_MODE)
sgx_sdk_get_setting(DEBUG_INT SONIC_DEMO_SGX_ACTIVE_DEBUG_FLAG)

add_dependencies(sonic_demo_sgx_host sonic_demo_sgx_enclave)

set_target_properties(sonic_demo_sgx_host PROPERTIES OUTPUT_NAME sonic_demo_sgx)

target_compile_definitions(sonic_demo_sgx_host
    PRIVATE
        SONIC_DEMO_SGX_DEBUG_FLAG=${SONIC_DEMO_SGX_ACTIVE_DEBUG_FLAG}
)
