add_executable(sonic_omap_demo)

target_sources(sonic_omap_demo PRIVATE
    app_omap.cpp
)

target_include_directories(sonic_omap_demo
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../../sonic_oram/demo
        ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(sonic_omap_demo
    PRIVATE
        sonic::omap.native
        sonic::omap::harness
        sonic::util
)

if(SONIC_ORAM_TIERED_STORAGE)
    target_compile_definitions(sonic_omap_demo PRIVATE SONIC_ORAM_TIERED_STORAGE=1)
endif()

set_target_properties(sonic_omap_demo PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
