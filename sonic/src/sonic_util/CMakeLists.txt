add_library(sonic_util INTERFACE)

target_include_directories(sonic_util
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(sonic_util
    INTERFACE
        sonic_common_options
)

if(WIN32)
    target_link_libraries(sonic_util INTERFACE ws2_32)
endif()

add_library(sonic::util ALIAS sonic_util)
