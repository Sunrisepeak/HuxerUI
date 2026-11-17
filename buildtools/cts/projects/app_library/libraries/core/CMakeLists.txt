huxerui_add_library(ctskit_core
        SOURCES
            src/core.cpp
        RESOURCES
            resources
        RESOURCE_NAMESPACE
            ctskit_core
)

target_include_directories(ctskit_core PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
add_library(CtsKit::Core ALIAS ctskit_core)
