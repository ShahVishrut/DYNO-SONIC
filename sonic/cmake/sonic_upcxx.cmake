set(SONIC_UPCXX_ROOT "" CACHE PATH "Path to a UPC++ installation prefix (contains share/cmake/UPCXX)")
set(SONIC_HAS_UPCXX OFF CACHE BOOL "Set when UPC++ has been located" FORCE)

if(NOT SONIC_BUILD_DISTRIBUTED)
    message(STATUS "SONIC_BUILD_DISTRIBUTED=OFF: UPC++ will not be used")
    return()
endif()

set(_sonic_upcxx_prefixes)
if(SONIC_UPCXX_ROOT)
    list(APPEND _sonic_upcxx_prefixes "${SONIC_UPCXX_ROOT}")
endif()
if(EXISTS "${CMAKE_SOURCE_DIR}/deps/upcxx-install")
    list(APPEND _sonic_upcxx_prefixes "${CMAKE_SOURCE_DIR}/deps/upcxx-install")
endif()
if(DEFINED ENV{UPCXX_INSTALL})
    list(APPEND _sonic_upcxx_prefixes "$ENV{UPCXX_INSTALL}")
endif()

foreach(_prefix IN LISTS _sonic_upcxx_prefixes)
    if(IS_DIRECTORY "${_prefix}")
        list(APPEND CMAKE_PREFIX_PATH "${_prefix}")
    endif()
endforeach()
unset(_prefix)
unset(_sonic_upcxx_prefixes)

find_package(UPCXX CONFIG QUIET)

if(NOT UPCXX_FOUND)
    message(FATAL_ERROR
        "SONIC_BUILD_DISTRIBUTED=ON but UPC++ was not found.\n"
        "  * Run scripts/setup_upcxx.py to install it locally, or\n"
        "  * Set SONIC_UPCXX_ROOT=/path/to/upcxx-install, or\n"
        "  * Disable SONIC_BUILD_DISTRIBUTED."
    )
endif()

set(SONIC_HAS_UPCXX ON CACHE BOOL "Set when UPC++ has been located" FORCE)
message(STATUS "Found UPC++: ${UPCXX_DIR}")
