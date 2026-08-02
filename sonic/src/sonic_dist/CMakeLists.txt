if(NOT SONIC_BUILD_DISTRIBUTED)
    message(STATUS "sonic_dist: distributed support disabled")
    return()
endif()

# select distributed backend
if(SONIC_DIST_BACKEND STREQUAL "MPI")
    include(${CMAKE_SOURCE_DIR}/cmake/sonic_mpi.cmake)
elseif(SONIC_DIST_BACKEND STREQUAL "UPCXX")
    include(${CMAKE_SOURCE_DIR}/cmake/sonic_upcxx.cmake)
else()
    message(FATAL_ERROR "sonic_dist: unknown SONIC_DIST_BACKEND='${SONIC_DIST_BACKEND}' (expected UPCXX or MPI)")
endif()

set(SONIC_DIST_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_library(sonic_dist STATIC)

# configure backend-specific implementation
if(SONIC_DIST_BACKEND STREQUAL "MPI")
    target_sources(sonic_dist PRIVATE
        ${SONIC_DIST_SOURCE_DIR}/sonic/dist/runtime_mpi.cpp
    )
    target_link_libraries(sonic_dist PRIVATE MPI::MPI_CXX)
    target_compile_definitions(sonic_dist PUBLIC SONIC_DIST_BACKEND_MPI=1)
elseif(SONIC_DIST_BACKEND STREQUAL "UPCXX")
    target_sources(sonic_dist PRIVATE
        ${SONIC_DIST_SOURCE_DIR}/sonic/dist/runtime_upcxx.cpp
    )
    target_link_libraries(sonic_dist PRIVATE UPCXX::upcxx)
    target_compile_definitions(sonic_dist PUBLIC SONIC_DIST_BACKEND_UPCXX=1)
endif()

target_include_directories(sonic_dist
    PUBLIC
        $<BUILD_INTERFACE:${SONIC_DIST_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
)

target_link_libraries(sonic_dist
    PUBLIC
        sonic_common_options
    PRIVATE
        sonic::threads
        sonic::util
)

target_compile_definitions(sonic_dist
    PUBLIC
        SONIC_DIST_ENABLED=1
)

add_library(sonic::dist ALIAS sonic_dist)
