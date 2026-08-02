add_executable(sonic_oram_demo)

target_sources(sonic_oram_demo PRIVATE
    app_oram.cpp
)

target_include_directories(sonic_oram_demo
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(sonic_oram_demo
    PRIVATE
        sonic::oram.native
        sonic::util
)

if(SONIC_ORAM_TIERED_STORAGE)
    target_compile_definitions(sonic_oram_demo PRIVATE SONIC_ORAM_TIERED_STORAGE=1)
endif()

set_target_properties(sonic_oram_demo PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
