include_guard(GLOBAL)

if(NOT SONIC_PLATFORM STREQUAL "sgx")
    return()
endif()

set(SONIC_DEMO_SGX_MODE "SIM" CACHE STRING "Execution mode for sonic_demo SGX enclave (HW or SIM)")
set_property(CACHE SONIC_DEMO_SGX_MODE PROPERTY STRINGS HW SIM)
option(SONIC_DEMO_SGX_DEBUG_ENCLAVE "Sign sonic_demo SGX enclave in debug mode" ON)
set(SONIC_DEMO_SGX_SIGNING_KEY "" CACHE FILEPATH "Optional PEM key used to sign sonic_demo SGX enclave")
set(SONIC_DEMO_SGX_PSW_DIR "" CACHE PATH "Optional Intel SGX PSW runtime directory")

sgx_sdk_init(
    MODE "${SONIC_DEMO_SGX_MODE}"
    DEBUG "${SONIC_DEMO_SGX_DEBUG_ENCLAVE}"
    SIGNING_KEY "${SONIC_DEMO_SGX_SIGNING_KEY}"
    PSW_DIR "${SONIC_DEMO_SGX_PSW_DIR}"
)
