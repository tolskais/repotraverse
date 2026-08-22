cmake_minimum_required(VERSION 3.24)

project(repotraverse VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(REPOTRAVERSE_CLANG_PROVIDER "package" CACHE STRING
    "Clang provider: package or disabled")
set_property(CACHE REPOTRAVERSE_CLANG_PROVIDER PROPERTY STRINGS package disabled)
set(REPOTRAVERSE_LLVM_ROOT "" CACHE PATH
    "Root of the prebuilt LLVM/Clang SDK")
option(REPOTRAVERSE_NATIVE_OPTIMIZATION
       "Optimize for the build VM CPU; the resulting executable is not portable"
       OFF)

if(WIN32 AND NOT (MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang"))
    message(FATAL_ERROR
        "Windows builds require clang-cl from REPOTRAVERSE_LLVM_ROOT; "
        "selected compiler: ${CMAKE_CXX_COMPILER_ID}")
endif()

if(REPOTRAVERSE_NATIVE_OPTIMIZATION)
    set(REPOTRAVERSE_BUILD_MODE "native")
else()
    set(REPOTRAVERSE_BUILD_MODE "generic")
endif()

add_library(repotraverse-build-options INTERFACE)
if(REPOTRAVERSE_NATIVE_OPTIMIZATION)
    if(MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        target_compile_options(repotraverse-build-options INTERFACE
            $<$<COMPILE_LANGUAGE:CXX>:/clang:-march=native>)
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        target_compile_options(repotraverse-build-options INTERFACE
            $<$<COMPILE_LANGUAGE:CXX>:-march=native>
            $<$<COMPILE_LANGUAGE:CXX>:-mtune=native>)
    else()
        message(FATAL_ERROR
            "Native optimization requires Clang; selected compiler: "
            "${CMAKE_CXX_COMPILER_ID}")
    endif()
endif()

# Pinned nlohmann/json v3.12.0 is vendored so configuration never consults a
# package manager or the network.
add_library(repotraverse-json INTERFACE)
add_library(nlohmann_json::nlohmann_json ALIAS repotraverse-json)
target_include_directories(repotraverse-json SYSTEM INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann_json/include")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_info.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/history/build_info.hpp"
    @ONLY)

add_library(history-core
    src/ir.cpp
    src/lineage.cpp
    src/query.cpp)
target_include_directories(history-core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(history-core
    PUBLIC nlohmann_json::nlohmann_json
    PRIVATE repotraverse-build-options)
target_compile_features(history-core PUBLIC cxx_std_20)

add_executable(repotraverse app/main.cpp)
target_link_libraries(repotraverse PRIVATE history-core repotraverse-build-options)

if(REPOTRAVERSE_CLANG_PROVIDER STREQUAL "package")
    if(REPOTRAVERSE_LLVM_ROOT)
        cmake_path(ABSOLUTE_PATH REPOTRAVERSE_LLVM_ROOT NORMALIZE)
        unset(_llvm_config CACHE)
        unset(_clang_config CACHE)
        find_file(_llvm_config LLVMConfig.cmake
            PATHS
                "${REPOTRAVERSE_LLVM_ROOT}/lib64/cmake/llvm"
                "${REPOTRAVERSE_LLVM_ROOT}/lib/cmake/llvm"
            NO_DEFAULT_PATH)
        find_file(_clang_config ClangConfig.cmake
            PATHS
                "${REPOTRAVERSE_LLVM_ROOT}/lib64/cmake/clang"
                "${REPOTRAVERSE_LLVM_ROOT}/lib/cmake/clang"
            NO_DEFAULT_PATH)
        if(NOT _llvm_config)
            message(FATAL_ERROR
                "REPOTRAVERSE_LLVM_ROOT does not contain LLVMConfig.cmake "
                "under lib/cmake/llvm or lib64/cmake/llvm: "
                "${REPOTRAVERSE_LLVM_ROOT}")
        endif()
        if(NOT _clang_config)
            message(FATAL_ERROR
                "REPOTRAVERSE_LLVM_ROOT does not contain ClangConfig.cmake "
                "under lib/cmake/clang or lib64/cmake/clang: "
                "${REPOTRAVERSE_LLVM_ROOT}")
        endif()
        cmake_path(GET _llvm_config PARENT_PATH _llvm_cmake)
        cmake_path(GET _clang_config PARENT_PATH _clang_cmake)
        find_package(LLVM CONFIG REQUIRED PATHS "${_llvm_cmake}" NO_DEFAULT_PATH)
        find_package(Clang CONFIG REQUIRED PATHS "${_clang_cmake}" NO_DEFAULT_PATH)
    else()
        # Connected developer builds may use an installed package. The Windows
        # offline presets and build script require REPOTRAVERSE_LLVM_ROOT.
        find_package(LLVM CONFIG REQUIRED)
        find_package(Clang CONFIG REQUIRED)
    endif()

    if(NOT TARGET clang-cpp OR NOT TARGET LLVM)
        message(FATAL_ERROR
            "The LLVM SDK must export the monolithic clang-cpp and LLVM CMake targets")
    endif()

    add_executable(clang-extractor extractor/main.cpp)
    target_include_directories(clang-extractor PRIVATE
        ${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})
    separate_arguments(_llvm_definitions NATIVE_COMMAND "${LLVM_DEFINITIONS}")
    target_compile_options(clang-extractor PRIVATE ${_llvm_definitions})
    target_link_libraries(clang-extractor PRIVATE
        history-core repotraverse-build-options clang-cpp LLVM)
elseif(NOT REPOTRAVERSE_CLANG_PROVIDER STREQUAL "disabled")
    message(FATAL_ERROR
        "Unknown REPOTRAVERSE_CLANG_PROVIDER=${REPOTRAVERSE_CLANG_PROVIDER}")
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(history-core-tests tests/core_tests.cpp)
    target_link_libraries(history-core-tests PRIVATE
        history-core repotraverse-build-options)
    target_compile_definitions(history-core-tests PRIVATE
        TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    add_test(NAME history-core-tests COMMAND history-core-tests)
    add_test(NAME repotraverse-version COMMAND repotraverse --version)
    set_tests_properties(repotraverse-version PROPERTIES
        PASS_REGULAR_EXPRESSION
            "repotraverse ${PROJECT_VERSION}.*build mode: ${REPOTRAVERSE_BUILD_MODE}")

    if(TARGET clang-extractor)
        add_test(NAME clang-extractor-version COMMAND clang-extractor --version)
        set_tests_properties(clang-extractor-version PROPERTIES
            PASS_REGULAR_EXPRESSION
                "repotraverse clang-extractor ${PROJECT_VERSION}.*LLVM/Clang")
        add_test(
            NAME clang-extractor-smoke
            COMMAND clang-extractor
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/sample.cpp"
                -- -std=c++17)
        set_tests_properties(clang-extractor-smoke PROPERTIES
            PASS_REGULAR_EXPRESSION "qualified_name")
        add_test(
            NAME clang-extractor-lineage
            COMMAND ${CMAKE_COMMAND}
                -DEXTRACTOR=$<TARGET_FILE:clang-extractor>
                -DREPOTRAVERSE=$<TARGET_FILE:repotraverse>
                -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
                -DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/extractor_integration.cmake)
    endif()
endif()

install(TARGETS history-core repotraverse
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib)
if(TARGET clang-extractor)
    install(TARGETS clang-extractor RUNTIME DESTINATION bin)
endif()
install(FILES third_party/nlohmann_json/LICENSE.MIT
    DESTINATION licenses/nlohmann_json)
