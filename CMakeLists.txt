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

function(openmoq_sanitize_target_link_directories target_name)
    if(NOT TARGET "${target_name}")
        return()
    endif()

    foreach(property_name IN ITEMS INTERFACE_LINK_DIRECTORIES LINK_DIRECTORIES)
        get_target_property(property_value "${target_name}" "${property_name}")
        if(NOT property_value OR property_value STREQUAL "property_value-NOTFOUND")
            continue()
        endif()

        set(filtered_value "${property_value}")
        list(FILTER filtered_value EXCLUDE REGEX "^\\($|^\\)$")
        set_target_properties("${target_name}" PROPERTIES "${property_name}" "${filtered_value}")
    endforeach()
endfunction()

if(OPENMOQ_ENABLE_PICOQUIC AND EXISTS "${OPENMOQ_PICOQUIC_SOURCE_DIR}/CMakeLists.txt")
    set(OPENMOQ_PICOTLS_SOURCE_DIR "${OPENMOQ_PICOQUIC_SOURCE_DIR}/../picotls" CACHE PATH
        "Path to a picotls source checkout")

    if(EXISTS "${OPENMOQ_PICOTLS_SOURCE_DIR}/CMakeLists.txt")
        # picotls still declares CMake 2.8 compatibility; newer CMake releases
        # require an explicit policy floor when configuring that subproject.
        set(CMAKE_POLICY_VERSION_MINIMUM "3.5" CACHE STRING "" FORCE)
        set(PICOQUIC_FETCH_PTLS ON CACHE BOOL "" FORCE)
        set(FETCHCONTENT_SOURCE_DIR_PICOTLS "${OPENMOQ_PICOTLS_SOURCE_DIR}" CACHE PATH "" FORCE)
        set(picoquic_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(BUILD_DEMO OFF CACHE BOOL "" FORCE)
        set(BUILD_HTTP ON CACHE BOOL "" FORCE)
        set(BUILD_PQBENCH OFF CACHE BOOL "" FORCE)
        set(BUILD_LOGLIB OFF CACHE BOOL "" FORCE)
        set(BUILD_LOGREADER OFF CACHE BOOL "" FORCE)
        if(APPLE)
            # picotls appends literal "(" and ")" list items when Brotli is
            # discovered via pkg-config. On macOS, CMake rejects the resulting
            # malformed source-prefixed link directories during generate. Scope
            # pkg-config away from system package metadata while picotls configures.
            set(_openmoq_saved_pkg_config_libdir "$ENV{PKG_CONFIG_LIBDIR}")
            set(_openmoq_saved_pkg_config_path "$ENV{PKG_CONFIG_PATH}")
            set(ENV{PKG_CONFIG_LIBDIR} "${CMAKE_CURRENT_BINARY_DIR}/openmoq-empty-pkgconfig")
            set(ENV{PKG_CONFIG_PATH} "${CMAKE_CURRENT_BINARY_DIR}/openmoq-empty-pkgconfig")
            file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/openmoq-empty-pkgconfig")
        endif()
        add_subdirectory("${OPENMOQ_PICOQUIC_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/picoquic")
        if(APPLE)
            set(ENV{PKG_CONFIG_LIBDIR} "${_openmoq_saved_pkg_config_libdir}")
            set(ENV{PKG_CONFIG_PATH} "${_openmoq_saved_pkg_config_path}")
            unset(_openmoq_saved_pkg_config_libdir)
            unset(_openmoq_saved_pkg_config_path)
        endif()
        # picotls currently injects literal "(" and ")" entries into its link
        # directories list when Brotli is discovered via pkg-config. CMake on
        # macOS rejects those malformed source-prefixed paths during generate.
        openmoq_sanitize_target_link_directories(picotls-core)
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
    src/transport/webtransport_client.cpp
)

target_include_directories(openmoq_publisher_lib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(openmoq_publisher_lib PUBLIC cxx_std_20)

if(OPENMOQ_HAS_PICOQUIC)
    target_compile_definitions(openmoq_publisher_lib PRIVATE OPENMOQ_HAS_PICOQUIC=1)
    target_link_libraries(openmoq_publisher_lib PRIVATE picoquic-core)
    if(TARGET picohttp-core)
        target_link_libraries(openmoq_publisher_lib PRIVATE picohttp-core)
        target_include_directories(openmoq_publisher_lib PRIVATE "${OPENMOQ_PICOQUIC_SOURCE_DIR}/picohttp")
    endif()
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

    add_executable(openmoq-publisher-webtransport-tests
        tests/webtransport_client_test.cpp
    )
    target_link_libraries(openmoq-publisher-webtransport-tests PRIVATE openmoq_publisher_lib)
    add_test(NAME openmoq-publisher-webtransport-tests COMMAND openmoq-publisher-webtransport-tests)

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
