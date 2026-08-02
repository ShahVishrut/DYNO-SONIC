add_library(sonic_obliv_interface INTERFACE)
target_include_directories(sonic_obliv_interface
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(sonic_obliv_interface
    INTERFACE
        sonic::util
        sonic_common_options
)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|X86_64|amd64|AMD64)$")
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(sonic_obliv_interface INTERFACE -masm=intel)
    endif()
endif()

add_library(sonic_obliv_native INTERFACE)
target_link_libraries(sonic_obliv_native INTERFACE sonic_obliv_interface)

add_library(sonic_obliv_no_os INTERFACE)
target_link_libraries(sonic_obliv_no_os INTERFACE sonic_obliv_interface)
target_compile_definitions(sonic_obliv_no_os INTERFACE SONIC_NO_OS=1)

add_library(sonic_obliv INTERFACE)
target_link_libraries(sonic_obliv INTERFACE
    $<IF:$<STREQUAL:$<TARGET_PROPERTY:SONIC_PLATFORM>,sgx>,sonic_obliv_no_os,sonic_obliv_native>
)

add_library(sonic::obliv ALIAS sonic_obliv)
add_library(sonic::obliv.native ALIAS sonic_obliv_native)
add_library(sonic::obliv.no_os ALIAS sonic_obliv_no_os)
