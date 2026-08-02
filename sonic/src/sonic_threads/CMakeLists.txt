add_library(sonic_threads_interface INTERFACE)
target_include_directories(sonic_threads_interface
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
        $<INSTALL_INTERFACE:include>
)
target_compile_definitions(sonic_threads_interface
    INTERFACE
        $<$<CONFIG:Debug>:SONIC_THREADS_DEBUG=1>
)
target_link_libraries(sonic_threads_interface INTERFACE sonic_common_options sonic::util)

add_library(sonic_threads_native INTERFACE)
target_link_libraries(sonic_threads_native INTERFACE sonic_threads_interface Threads::Threads)
target_compile_definitions(sonic_threads_native INTERFACE
    SONIC_THREADS_HAS_OS=1
    SONIC_THREADS_HAS_CHRONO=1
)

add_library(sonic_threads_no_os INTERFACE)
target_link_libraries(sonic_threads_no_os INTERFACE sonic_threads_interface)
target_compile_definitions(sonic_threads_no_os INTERFACE
    SONIC_THREADS_HAS_OS=0
    SONIC_THREADS_HAS_CHRONO=0
)

add_library(sonic_threads_sgx INTERFACE)
target_link_libraries(sonic_threads_sgx INTERFACE sonic_threads_interface)
target_compile_definitions(sonic_threads_sgx INTERFACE
    SONIC_THREADS_HAS_OS=0
    SONIC_THREADS_HAS_CHRONO=0
)
sonic_target_set_platform(sonic_threads_sgx sgx)

add_library(sonic_threads INTERFACE)
target_link_libraries(sonic_threads INTERFACE
    $<IF:$<STREQUAL:$<TARGET_PROPERTY:SONIC_PLATFORM>,sgx>,sonic_threads_sgx,sonic_threads_native>
)

add_library(sonic::threads ALIAS sonic_threads)
add_library(sonic::threads.native ALIAS sonic_threads_native)
add_library(sonic::threads.no_os ALIAS sonic_threads_no_os)
add_library(sonic::threads.sgx ALIAS sonic_threads_sgx)
