set(SONIC_DEMO_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(SONIC_DEMO_COMMON_DIR ${SONIC_DEMO_DIR}/common)
set(SONIC_DEMO_LOGIC_DIR ${SONIC_DEMO_DIR}/logic)
set(SONIC_DEMO_CLI_DIR ${SONIC_DEMO_DIR}/cli)
set(SONIC_DEMO_NATIVE_DIR ${SONIC_DEMO_DIR}/native)

add_library(sonic_demo_common INTERFACE)
target_include_directories(sonic_demo_common
    INTERFACE
        $<BUILD_INTERFACE:${SONIC_DEMO_COMMON_DIR}>
)
target_link_libraries(sonic_demo_common INTERFACE sonic_common_options)

add_library(sonic_demo_logic INTERFACE)
target_include_directories(sonic_demo_logic
    INTERFACE
        $<BUILD_INTERFACE:${SONIC_DEMO_LOGIC_DIR}>
        $<BUILD_INTERFACE:${SONIC_DEMO_COMMON_DIR}>
)
target_link_libraries(sonic_demo_logic
    INTERFACE
        sonic_demo_common
        sonic::sgxbridge.common
        sonic::util
)
target_compile_definitions(sonic_demo_logic INTERFACE SONIC_ORAM_TIERED_STORAGE=1)

add_library(sonic_demo_cli STATIC
    ${SONIC_DEMO_CLI_DIR}/sonic/demo/cli/parser.cpp
)
target_include_directories(sonic_demo_cli
    PUBLIC
        $<BUILD_INTERFACE:${SONIC_DEMO_CLI_DIR}>
        $<BUILD_INTERFACE:${SONIC_DEMO_COMMON_DIR}>
)
target_link_libraries(sonic_demo_cli
    PUBLIC
        sonic_demo_common
        sonic::oram::harness
        sonic::omap::harness
        sonic::util
)

add_executable(sonic_demo_native
    ${SONIC_DEMO_NATIVE_DIR}/main.cpp
)
target_link_libraries(sonic_demo_native
    PRIVATE
        sonic_demo_logic
        sonic_demo_cli
        sonic::oram::harness
        sonic::omap::harness
        sonic::sgxbridge.common
        sonic::threads
        sonic::util
)

if(SONIC_PLATFORM STREQUAL "native")
    set_target_properties(sonic_demo_native PROPERTIES OUTPUT_NAME sonic_demo)
endif()

if(SONIC_PLATFORM STREQUAL "sgx")
    add_subdirectory(sgx)
endif()
