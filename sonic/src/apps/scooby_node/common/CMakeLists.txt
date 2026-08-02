if(TARGET scooby_node_common)
    return()
endif()

set(SCOOBY_NODE_COMMON_DIR ${CMAKE_CURRENT_SOURCE_DIR})

add_library(scooby_node_types INTERFACE)
target_include_directories(scooby_node_types
    INTERFACE
        $<BUILD_INTERFACE:${SCOOBY_NODE_COMMON_DIR}>
)
target_link_libraries(scooby_node_types
    INTERFACE
        sonic::sgxbridge.common
        sonic::obliv
        sonic::util
)

add_library(scooby_node_common INTERFACE)
target_include_directories(scooby_node_common
    INTERFACE
        $<BUILD_INTERFACE:${SCOOBY_NODE_COMMON_DIR}>
)
target_link_libraries(scooby_node_common
    INTERFACE
        scooby_node_types
        sonic_common_options
)

