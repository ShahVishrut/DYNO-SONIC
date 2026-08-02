
set(SONIC_PLATFORM "native" CACHE STRING "Set the global build platform (e.g., native, sgx)")
set_property(CACHE SONIC_PLATFORM PROPERTY STRINGS native sgx)

message(STATUS "Configuring Sonic project for '${SONIC_PLATFORM}' platform.")

function(sonic_target_set_platform target platform)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "sonic_target_set_platform: target '${target}' not found.")
    endif()
    set_property(TARGET "${target}" PROPERTY SONIC_PLATFORM "${platform}")
endfunction()
