
# enable march/mtune native in release builds
if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    if(NOT MSVC)
        add_compile_options(-O3 -march=native -mtune=native)
    endif()
endif()

# conditionally enable/disable avx512
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
    if(SONIC_ENABLE_AVX512)
        if(NOT MSVC)
            add_compile_options(-mavx512f -mavx512vl -mavx512bw -mavx512dq)
        else()
            add_compile_options(/arch:AVX512)
        endif()
    else()
        if(NOT MSVC)
            add_compile_options(
                -mno-avx512f
                -mno-avx512vl
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
        endif()
    endif()
endif()
