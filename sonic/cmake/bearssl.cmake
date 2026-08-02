include_guard(GLOBAL)

include(FetchContent)
include(CheckCCompilerFlag)

if(NOT CMAKE_C_COMPILER_LOADED)
    enable_language(C)
endif()

if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()

set(BEARSSL_VERSION "v0.6" CACHE STRING "BearSSL release tag")

function(bearssl_add_library target)
    set(options ENABLE_SHA256 ENABLE_AES_GCM)
    cmake_parse_arguments(BEARSSL "${options}" "" "" ${ARGN})

    set(_bearssl_arch_candidates)
    if(APPLE AND CMAKE_OSX_ARCHITECTURES)
        set(_bearssl_arch_candidates "${CMAKE_OSX_ARCHITECTURES}")
    endif()
    if(NOT _bearssl_arch_candidates AND CMAKE_C_COMPILER_TARGET)
        set(_bearssl_arch_candidates "${CMAKE_C_COMPILER_TARGET}")
    endif()
    if(NOT _bearssl_arch_candidates AND CMAKE_SYSTEM_PROCESSOR)
        set(_bearssl_arch_candidates "${CMAKE_SYSTEM_PROCESSOR}")
    endif()
    if(NOT _bearssl_arch_candidates AND CMAKE_HOST_SYSTEM_PROCESSOR)
        set(_bearssl_arch_candidates "${CMAKE_HOST_SYSTEM_PROCESSOR}")
    endif()

    set(_bearssl_is_x86 OFF)
    if(_bearssl_arch_candidates)
        foreach(_bearssl_arch IN LISTS _bearssl_arch_candidates)
            string(TOLOWER "${_bearssl_arch}" _bearssl_arch_lower)
            if(_bearssl_arch_lower MATCHES "x86_64|amd64|i[3-6]86")
                set(_bearssl_is_x86 ON)
                break()
            endif()
        endforeach()
    endif()

    set(_bearssl_use_x86ni OFF)
    if(_bearssl_is_x86)
        set(_bearssl_use_x86ni ON)
    endif()

    FetchContent_Declare(bearssl
        GIT_REPOSITORY https://www.bearssl.org/git/BearSSL
        GIT_TAG "${BEARSSL_VERSION}"
        GIT_SHALLOW TRUE
        SOURCE_DIR "${CMAKE_BINARY_DIR}/_deps/bearssl-src"
    )

    FetchContent_GetProperties(bearssl)
    if(NOT bearssl_POPULATED)
        FetchContent_Populate(bearssl)

        if(_bearssl_use_x86ni)
            set(_aes_file "${bearssl_SOURCE_DIR}/src/symcipher/aes_x86ni.c")
            set(_ghash_file "${bearssl_SOURCE_DIR}/src/hash/ghash_pclmul.c")
            file(READ "${_aes_file}" _bearssl_aes_source)
            file(READ "${_ghash_file}" _bearssl_ghash_source)
            set(_bearssl_aes_source_patched "${_bearssl_aes_source}")
            string(REGEX REPLACE "return[ \t]+br_cpuid\\([^;]*\\);" "return 1;" _bearssl_aes_source_patched "${_bearssl_aes_source_patched}")
            if(NOT _bearssl_aes_source_patched STREQUAL _bearssl_aes_source)
                file(WRITE "${_aes_file}" "${_bearssl_aes_source_patched}")
            endif()

            set(_bearssl_ghash_source_patched "${_bearssl_ghash_source}")
            string(REGEX REPLACE "return[ \t]+pclmul_supported\\([^;]*\\);" "return &br_ghash_pclmul;" _bearssl_ghash_source_patched "${_bearssl_ghash_source_patched}")
            if(NOT _bearssl_ghash_source_patched STREQUAL _bearssl_ghash_source)
                file(WRITE "${_ghash_file}" "${_bearssl_ghash_source_patched}")
            endif()
        endif()
    endif()

    set(_root "${bearssl_SOURCE_DIR}")
    set(_sources
        "${_root}/src/codec/dec32be.c"
        "${_root}/src/codec/dec32le.c"
        "${_root}/src/codec/enc32be.c"
        "${_root}/src/codec/enc32le.c"
        "${_root}/src/codec/enc64be.c"
    )

    if(BEARSSL_ENABLE_SHA256 OR BEARSSL_ENABLE_AES_GCM)
        list(APPEND _sources "${_root}/src/hash/sha2small.c")
    endif()

    list(APPEND _sources "${_root}/src/symcipher/aes_common.c")
    if(_bearssl_use_x86ni)
        list(APPEND _sources
            "${_root}/src/symcipher/aes_x86ni.c"
            "${_root}/src/symcipher/aes_x86ni_ctr.c"
        )
        if(BEARSSL_ENABLE_AES_GCM)
            list(APPEND _sources
                "${_root}/src/symcipher/aes_x86ni_cbcenc.c"
                "${_root}/src/symcipher/aes_x86ni_cbcdec.c"
                "${_root}/src/symcipher/aes_x86ni_ctrcbc.c"
                "${_root}/src/hash/ghash_pclmul.c"
            )
        endif()
    else()
        list(APPEND _sources
            "${_root}/src/symcipher/aes_ct64.c"
            "${_root}/src/symcipher/aes_ct64_enc.c"
            "${_root}/src/symcipher/aes_ct64_dec.c"
            "${_root}/src/symcipher/aes_ct64_ctr.c"
        )
        if(BEARSSL_ENABLE_AES_GCM)
            list(APPEND _sources
                "${_root}/src/symcipher/aes_ct64_cbcenc.c"
                "${_root}/src/symcipher/aes_ct64_cbcdec.c"
                "${_root}/src/symcipher/aes_ct64_ctrcbc.c"
                "${_root}/src/hash/ghash_ctmul.c"
            )
        endif()
    endif()

    if(BEARSSL_ENABLE_AES_GCM)
        list(APPEND _sources "${_root}/src/aead/gcm.c")
    endif()

    list(APPEND _sources
        "${_root}/src/kdf/hkdf.c"
        "${_root}/src/mac/hmac.c"
    )

    add_library(${target} STATIC ${_sources})
    target_include_directories(${target}
        PUBLIC "${_root}/inc"
        PRIVATE "${_root}/src"
    )
    set(_bearssl_common_flags
        -fPIC
        -O3
        -fomit-frame-pointer
        -funroll-loops
    )
    target_compile_options(${target} PRIVATE ${_bearssl_common_flags})

    if(_bearssl_use_x86ni)
        set(_bearssl_candidate_flags
            -march=native
            -mtune=native
            -msse2
            -mssse3
            -maes
            -mpclmul
            -mno-avx512f
            -mno-avx512vl
            -mno-avx512pf
            -mno-avx512er
            -mno-avx512cd
            -mno-avx512bw
            -mno-avx512dq
            -mno-avx512ifma
            -mno-avx512vbmi
            -mno-avx512vbmi2
            -mno-avx512vnni
            -mno-avx512bitalg
            -mno-avx512vpopcntdq
        )
    else()
        set(_bearssl_candidate_flags
            -march=native
            -mtune=native
        )
    endif()
    foreach(_bearssl_flag IN LISTS _bearssl_candidate_flags)
        string(REGEX REPLACE "^-+" "" _bearssl_flag_key "${_bearssl_flag}")
        string(REGEX REPLACE "[^A-Za-z0-9_]" "_" _bearssl_flag_key "${_bearssl_flag_key}")
        string(TOUPPER "${_bearssl_flag_key}" _bearssl_flag_key)
        set(_bearssl_flag_cache "BEARSSL_HAVE_FLAG_${_bearssl_flag_key}")
        check_c_compiler_flag("${_bearssl_flag}" ${_bearssl_flag_cache})
        if(${_bearssl_flag_cache})
            target_compile_options(${target} PRIVATE "${_bearssl_flag}")
        endif()
    endforeach()
    set(_bearssl_private_defs
        _FORTIFY_SOURCE=0
        BR_ENABLE_INTRINSICS=1
    )
    if(_bearssl_use_x86ni)
        list(APPEND _bearssl_private_defs BR_AES_X86NI=1)
        target_compile_definitions(${target} PUBLIC SN_BEARSSL_USE_X86NI=1)
    else()
        list(APPEND _bearssl_private_defs BR_AES_CT64=1)
        message(WARNING "Using software AES implementation (CT64)")
        target_compile_definitions(${target} PUBLIC SN_BEARSSL_USE_CT64=1)
    endif()
    target_compile_definitions(${target} PRIVATE ${_bearssl_private_defs})
endfunction()
