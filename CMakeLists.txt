cmake_minimum_required(VERSION 3.20)

project(openmoq_publisher
    VERSION 0.1.0
    DESCRIPTION "OpenMOQ contribution project: C++20 publisher for fragmented MP4 to CMSF/MOQT"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(OPENMOQ_BUILD_TESTS "Build OpenMOQ publisher tests" ON)

add_library(openmoq_publisher_lib
    src/cli_options.cpp
    src/cmaf_segmenter.cpp
    src/cmsf_packager.cpp
    src/moq_draft.cpp
    src/mp4_box.cpp
)

target_include_directories(openmoq_publisher_lib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(openmoq_publisher_lib PUBLIC cxx_std_20)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(openmoq_publisher_lib PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(openmoq-publisher src/main.cpp)
target_link_libraries(openmoq-publisher PRIVATE openmoq_publisher_lib)

include(CTest)

if(OPENMOQ_BUILD_TESTS)
    add_executable(openmoq-publisher-tests
        tests/cmaf_segmenter_test.cpp
    )
    target_link_libraries(openmoq-publisher-tests PRIVATE openmoq_publisher_lib)
    add_test(NAME openmoq-publisher-tests COMMAND openmoq-publisher-tests)
endif()
