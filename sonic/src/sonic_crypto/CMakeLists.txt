option(SONIC_CRYPTO_TEE "Disable redundant secure wipes to TEE private memory" ON)
set(SONIC_CRYPTO_BACKEND "openssl" CACHE STRING "Crypto backend to use (openssl or bearssl)")
set_property(CACHE SONIC_CRYPTO_BACKEND PROPERTY STRINGS openssl bearssl)

option(SONIC_OPENSSL_USE_LOWLEVEL "Use low-level OpenSSL primitives for speed" ON)

set(_sonic_crypto_openssl_sources
    src/sonic/crypto/impl/openssl/random.cpp
    src/sonic/crypto/impl/openssl/prng.cpp
    src/sonic/crypto/impl/openssl/cipher.cpp
    src/sonic/crypto/impl/openssl/prf.cpp
    src/sonic/crypto/impl/openssl/prf_backend_lowlevel.cpp
    src/sonic/crypto/impl/openssl/prf_backend_evp.cpp
    src/sonic/crypto/impl/openssl/hash.cpp
    src/sonic/crypto/impl/openssl/hkdf.cpp
    src/sonic/crypto/impl/openssl/detail/openssl_utils.cpp
)

set(_sonic_crypto_bearssl_sources
    src/sonic/crypto/impl/bearssl/random.cpp
    src/sonic/crypto/impl/bearssl/prng.cpp
    src/sonic/crypto/impl/bearssl/cipher.cpp
    src/sonic/crypto/impl/bearssl/prf.cpp
    src/sonic/crypto/impl/bearssl/prf_backend.cpp
    src/sonic/crypto/impl/bearssl/hkdf.cpp
)

add_library(sonic_crypto_native STATIC)
target_include_directories(sonic_crypto_native PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(sonic_crypto_native PUBLIC sonic_common_options sonic::util)

if(SONIC_CRYPTO_TEE)
    target_compile_definitions(sonic_crypto_native PUBLIC SONIC_CRYPTO_TEE)
endif()

set(_sonic_crypto_platform_requires_bearssl OFF)
if(SONIC_PLATFORM STREQUAL "sgx")
    set(_sonic_crypto_platform_requires_bearssl ON)
endif()

set(_sonic_crypto_backend_uses_bearssl OFF)
if(SONIC_CRYPTO_BACKEND STREQUAL "bearssl")
    set(_sonic_crypto_backend_uses_bearssl ON)
endif()

set(_sonic_crypto_fetch_bearssl ${_sonic_crypto_backend_uses_bearssl})
if(_sonic_crypto_platform_requires_bearssl)
    set(_sonic_crypto_fetch_bearssl ON)
endif()

if(_sonic_crypto_fetch_bearssl)
    include(${CMAKE_SOURCE_DIR}/cmake/bearssl.cmake)
endif()

if(SONIC_CRYPTO_BACKEND STREQUAL "openssl")
    find_package(OpenSSL REQUIRED)
    target_sources(sonic_crypto_native PRIVATE ${_sonic_crypto_openssl_sources})
    target_compile_definitions(sonic_crypto_native PUBLIC SONIC_CRYPTO_BACKEND_OPENSSL=1)
    target_link_libraries(sonic_crypto_native PUBLIC OpenSSL::Crypto)
    if(SONIC_OPENSSL_USE_LOWLEVEL)
        target_compile_definitions(sonic_crypto_native PUBLIC SONIC_OPENSSL_USE_LOWLEVEL=1)
    else()
        target_compile_definitions(sonic_crypto_native PUBLIC SONIC_OPENSSL_USE_LOWLEVEL=0)
    endif()
elseif(SONIC_CRYPTO_BACKEND STREQUAL "bearssl")
    if(NOT TARGET bearssl_native)
        bearssl_add_library(bearssl_native ENABLE_SHA256 ENABLE_AES_GCM)
    endif()
    target_sources(sonic_crypto_native PRIVATE ${_sonic_crypto_bearssl_sources})
    target_compile_definitions(sonic_crypto_native PUBLIC SONIC_CRYPTO_BACKEND_BEARSSL=1)
    target_link_libraries(sonic_crypto_native PUBLIC bearssl_native)
else()
    message(FATAL_ERROR "Unsupported SONIC_CRYPTO_BACKEND: '${SONIC_CRYPTO_BACKEND}'.")
endif()

if(_sonic_crypto_platform_requires_bearssl)
    if(NOT TARGET bearssl_sgx)
        bearssl_add_library(bearssl_sgx ENABLE_SHA256 ENABLE_AES_GCM)
    endif()
    if(SONIC_PLATFORM STREQUAL "sgx")
        get_target_property(_bearssl_sgx_type bearssl_sgx TYPE)
        if(_bearssl_sgx_type AND NOT _bearssl_sgx_type STREQUAL "INTERFACE_LIBRARY")
            sgx_sdk_configure_trusted_target(bearssl_sgx)
        endif()
    endif()
elseif(NOT TARGET bearssl_sgx)
    add_library(bearssl_sgx INTERFACE)
endif()

set(_sonic_crypto_optimize_options)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    list(APPEND _sonic_crypto_optimize_options
        -O3
        -fomit-frame-pointer
        -funroll-loops
    )
endif()
if(_sonic_crypto_optimize_options)
    target_compile_options(sonic_crypto_native PRIVATE ${_sonic_crypto_optimize_options})
endif()

set(_sonic_crypto_sgx_private_compile_options ${_sonic_crypto_optimize_options})
get_target_property(_sonic_crypto_bearssl_defs bearssl_sgx INTERFACE_COMPILE_DEFINITIONS)
if(_sonic_crypto_bearssl_defs)
    list(JOIN _sonic_crypto_bearssl_defs ";" _sonic_crypto_bearssl_defs_joined)
    if(_sonic_crypto_bearssl_defs_joined MATCHES "(^|;)SN_BEARSSL_USE_X86NI=1($|;)")
        list(APPEND _sonic_crypto_sgx_private_compile_options
            -msse2
            -mssse3
            -msse4.1
            -maes
            -mpclmul
        )
    endif()
endif()

set(_sonic_crypto_sgx_public_includes
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include>
)
set(_sonic_crypto_sgx_public_links
    sonic_common_options
    sonic::util
    bearssl_sgx
)
set(_sonic_crypto_sgx_public_definitions
    SONIC_CRYPTO_BACKEND_BEARSSL=1
    SONIC_CRYPTO_SGX=1
)
if(SONIC_CRYPTO_TEE)
    list(APPEND _sonic_crypto_sgx_public_definitions SONIC_CRYPTO_TEE)
endif()

if(NOT TARGET sonic_crypto_sgx)
    add_library(sonic_crypto_sgx INTERFACE)
endif()
target_include_directories(sonic_crypto_sgx INTERFACE ${_sonic_crypto_sgx_public_includes})
target_link_libraries(sonic_crypto_sgx INTERFACE ${_sonic_crypto_sgx_public_links})
target_compile_definitions(sonic_crypto_sgx INTERFACE ${_sonic_crypto_sgx_public_definitions})

if(SONIC_PLATFORM STREQUAL "sgx")
    set(_sonic_crypto_bearssl_abs_sources)
    foreach(_src IN LISTS _sonic_crypto_bearssl_sources)
        list(APPEND _sonic_crypto_bearssl_abs_sources "${CMAKE_CURRENT_SOURCE_DIR}/${_src}")
    endforeach()
    sgx_sdk_add_trusted_variant(sonic_crypto_sgx
        SOURCES ${_sonic_crypto_bearssl_abs_sources}
        PRIVATE_COMPILE_OPTIONS ${_sonic_crypto_sgx_private_compile_options}
    )
endif()

sonic_target_set_platform(sonic_crypto_sgx sgx)

add_library(sonic_crypto INTERFACE)
target_link_libraries(sonic_crypto INTERFACE
    $<IF:$<STREQUAL:$<TARGET_PROPERTY:SONIC_PLATFORM>,sgx>,sonic_crypto_sgx,sonic_crypto_native>
)

add_library(sonic::crypto ALIAS sonic_crypto)
add_library(sonic::crypto.native ALIAS sonic_crypto_native)
add_library(sonic::crypto.sgx ALIAS sonic_crypto_sgx)
