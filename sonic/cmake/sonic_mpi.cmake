set(SONIC_HAS_MPI OFF CACHE BOOL "Set when MPI has been located" FORCE)

if(NOT SONIC_BUILD_DISTRIBUTED)
    message(STATUS "SONIC_BUILD_DISTRIBUTED=OFF: MPI will not be used")
    return()
endif()

find_package(MPI REQUIRED)

if(NOT MPI_FOUND)
    message(FATAL_ERROR
        "SONIC_DIST_BACKEND=MPI but MPI was not found.\n"
        "  * Install an MPI implementation (e.g., OpenMPI or MPICH), or\n"
        "  * Disable distributed targets with SONIC_BUILD_DISTRIBUTED=OFF."
    )
endif()

set(SONIC_HAS_MPI ON CACHE BOOL "Set when MPI has been located" FORCE)
message(STATUS "Found MPI: ${MPI_CXX_COMPILER}")
