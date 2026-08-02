option(SONIC_BUILD_APPLICATIONS "build application programs" OFF)
option(SONIC_ENABLE_LOGGING "enable detailed logging" ON)
option(SONIC_ENABLE_SANITIZERS "enable address/undefined behaviour sanitizers" OFF)
option(SONIC_ORAM_DEBUG "enable additional ORAM debug checks/logging" OFF)
option(SONIC_ORAM_METRICS "enable ORAM metrics counters" OFF)
option(SONIC_ORAM_TIERED_STORAGE "enable ORAM tiered storage variants" OFF)
option(SONIC_ENABLE_AVX512 "enable AVX-512 instructions" OFF)
option(SONIC_BUILD_DISTRIBUTED "build distributed targets" OFF)
set(SONIC_DIST_BACKEND "MPI" CACHE STRING "Distributed backend implementation (UPCXX or MPI)")
set_property(CACHE SONIC_DIST_BACKEND PROPERTY STRINGS "UPCXX" "MPI")

include(cmake/optflags.cmake)

if(WIN32)
    add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_definitions(SONIC_DEBUG_BUILD)
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g" CACHE STRING "Debug flags" FORCE)
    set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -g" CACHE STRING "Debug flags" FORCE)
endif()

if(SONIC_ENABLE_LOGGING)
    add_compile_definitions(ENABLE_LOGGING)
endif()

if(SONIC_ENABLE_SANITIZERS)
    if(MSVC)
        message(FATAL_ERROR "SONIC_ENABLE_SANITIZERS is not supported on MSVC")
    endif()

    set(SONIC_SANITIZER_FLAG "-fsanitize=address,undefined")
    set(SONIC_SANITIZER_COMPILE_EXTRAS
        "-fno-omit-frame-pointer"
        "-fno-sanitize-recover=undefined"
    )
endif()
