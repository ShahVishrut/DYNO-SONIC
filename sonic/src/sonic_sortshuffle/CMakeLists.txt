add_library(sonic_sortshuffle_interface INTERFACE)
target_include_directories(sonic_sortshuffle_interface
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(sonic_sortshuffle_interface INTERFACE sonic::util sonic::obliv sonic_common_options)

add_library(sonic_sortshuffle INTERFACE)
target_link_libraries(sonic_sortshuffle INTERFACE sonic_sortshuffle_interface sonic::threads)
add_library(sonic::sortshuffle ALIAS sonic_sortshuffle)
