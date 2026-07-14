cmake_minimum_required(VERSION 3.20)

project(VirtualMicrophone VERSION 0.1.0 LANGUAGES CXX)

add_executable(virtual-microphone
    src/main.cpp
    src/vst2_host.cpp
)

target_compile_features(virtual-microphone PRIVATE cxx_std_20)
target_compile_definitions(virtual-microphone PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
if (MSVC)
    target_compile_options(virtual-microphone PRIVATE /W4 /permissive-)
else()
    target_compile_options(virtual-microphone PRIVATE -Wall -Wextra -Wpedantic)
endif()
target_include_directories(virtual-microphone PRIVATE
    vendor/noise-suppression-for-voice/external/FST/fst
)
target_link_libraries(virtual-microphone PRIVATE
    ole32
    uuid
    avrt
    propsys
    ksuser
)
if (MINGW)
    target_link_options(virtual-microphone PRIVATE
        -municode
        -static
        -static-libgcc
        -static-libstdc++
    )
endif()

set_target_properties(virtual-microphone PROPERTIES
    OUTPUT_NAME "virtual-microphone"
)
