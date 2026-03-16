cmake_minimum_required(VERSION 3.20)

project(openmoq_publisher
    VERSION 0.1.0
    DESCRIPTION "OpenMOQ contribution project: C++20 publisher for fragmented MP4 to CMSF/MOQT"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(OPENMOQ_BUILD_TESTS "Build OpenMOQ publisher tests" ON)
option(OPENMOQ_ENABLE_PICOQUIC "Enable picoquic transport integration when picoquic is available" ON)
option(OPENMOQ_RUN_PICOQUIC_SMOKE_TESTS "Build and run picoquic loopback smoke tests" OFF)

set(OPENMOQ_PICOQUIC_SOURCE_DIR "/media/mondain/terrorbyte/workspace/github/picoquic" CACHE PATH
    "Path to a picoquic source checkout")

set(OPENMOQ_HAS_PICOQUIC OFF)

if(OPENMOQ_ENABLE_PICOQUIC AND EXISTS "${OPENMOQ_PICOQUIC_SOURCE_DIR}/CMakeLists.txt")
    set(OPENMOQ_PICOTLS_SOURCE_DIR "${OPENMOQ_PICOQUIC_SOURCE_DIR}/../picotls" CACHE PATH
        "Path to a picotls source checkout")

    if(EXISTS "${OPENMOQ_PICOTLS_SOURCE_DIR}/CMakeLists.txt")
        set(PICOQUIC_FETCH_PTLS ON CACHE BOOL "" FORCE)
        set(FETCHCONTENT_SOURCE_DIR_PICOTLS "${OPENMOQ_PICOTLS_SOURCE_DIR}" CACHE PATH "" FORCE)
        set(picoquic_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(BUILD_DEMO OFF CACHE BOOL "" FORCE)
        set(BUILD_HTTP OFF CACHE BOOL "" FORCE)
        set(BUILD_PQBENCH OFF CACHE BOOL "" FORCE)
        set(BUILD_LOGLIB OFF CACHE BOOL "" FORCE)
        set(BUILD_LOGREADER OFF CACHE BOOL "" FORCE)
        add_subdirectory("${OPENMOQ_PICOQUIC_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/picoquic")
        set(OPENMOQ_HAS_PICOQUIC ON)
    else()
        message(STATUS "picoquic checkout found, but picotls source was not found; building without picoquic integration")
    endif()
endif()

add_library(openmoq_publisher_lib
    src/cli_options.cpp
    src/cmaf_segmenter.cpp
    src/cmsf_packager.cpp
    src/moq_draft.cpp
    src/mp4_box.cpp
    src/transport/moqt_control_messages.cpp
    src/transport/moqt_session.cpp
    src/transport/picoquic_client.cpp
    src/transport/publisher_transport.cpp
)

target_include_directories(openmoq_publisher_lib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(openmoq_publisher_lib PUBLIC cxx_std_20)

if(OPENMOQ_HAS_PICOQUIC)
    target_compile_definitions(openmoq_publisher_lib PRIVATE OPENMOQ_HAS_PICOQUIC=1)
    target_link_libraries(openmoq_publisher_lib PRIVATE picoquic-core)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(openmoq_publisher_lib PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(openmoq-publisher src/main.cpp)
target_link_libraries(openmoq-publisher PRIVATE openmoq_publisher_lib)

include(CTest)

if(OPENMOQ_BUILD_TESTS)
    add_executable(openmoq-publisher-packaging-tests
        tests/cmaf_segmenter_test.cpp
    )
    target_link_libraries(openmoq-publisher-packaging-tests PRIVATE openmoq_publisher_lib)
    add_test(NAME openmoq-publisher-packaging-tests COMMAND openmoq-publisher-packaging-tests)

    add_executable(openmoq-publisher-cli-tests
        tests/cli_options_test.cpp
    )
    target_link_libraries(openmoq-publisher-cli-tests PRIVATE openmoq_publisher_lib)
    add_test(NAME openmoq-publisher-cli-tests COMMAND openmoq-publisher-cli-tests)

    add_executable(openmoq-publisher-transport-tests
        tests/moqt_session_test.cpp
    )
    target_link_libraries(openmoq-publisher-transport-tests PRIVATE openmoq_publisher_lib)
    add_test(NAME openmoq-publisher-transport-tests COMMAND openmoq-publisher-transport-tests)

    if(OPENMOQ_HAS_PICOQUIC AND OPENMOQ_RUN_PICOQUIC_SMOKE_TESTS)
        add_executable(openmoq-publisher-picoquic-smoke-tests
            tests/picoquic_smoke_test.cpp
        )
        target_link_libraries(openmoq-publisher-picoquic-smoke-tests PRIVATE openmoq_publisher_lib)
        target_include_directories(openmoq-publisher-picoquic-smoke-tests PRIVATE
            "${OPENMOQ_PICOQUIC_SOURCE_DIR}/picoquic"
        )
        add_test(NAME openmoq-publisher-picoquic-smoke-tests COMMAND openmoq-publisher-picoquic-smoke-tests)
    endif()
endif()
