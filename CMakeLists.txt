cmake_minimum_required(VERSION 3.24)

project(semantic_history VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(ANALYSIS_CLANG_PROVIDER "package" CACHE STRING
    "Clang provider: package, source, or disabled")
set_property(CACHE ANALYSIS_CLANG_PROVIDER PROPERTY STRINGS package source disabled)

find_package(nlohmann_json 3.11 REQUIRED)

add_library(history-core
    src/ir.cpp
    src/lineage.cpp
    src/query.cpp)
target_include_directories(history-core PUBLIC include)
target_link_libraries(history-core PUBLIC nlohmann_json::nlohmann_json)
target_compile_features(history-core PUBLIC cxx_std_20)

add_executable(analysis-tool app/main.cpp)
target_link_libraries(analysis-tool PRIVATE history-core)

if(ANALYSIS_CLANG_PROVIDER STREQUAL "package")
    find_package(LLVM CONFIG REQUIRED)
    find_package(Clang CONFIG REQUIRED)
    add_executable(clang-extractor extractor/main.cpp)
    target_include_directories(clang-extractor PRIVATE ${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})
    target_compile_definitions(clang-extractor PRIVATE ${LLVM_DEFINITIONS})
    target_link_libraries(clang-extractor PRIVATE history-core clang-cpp LLVM)
elseif(ANALYSIS_CLANG_PROVIDER STREQUAL "source")
    if(NOT TARGET clang-cpp)
        message(FATAL_ERROR
            "source provider must be configured as an LLVM external project with clang enabled")
    endif()
    add_executable(clang-extractor extractor/main.cpp)
    target_link_libraries(clang-extractor PRIVATE history-core clang-cpp LLVM)
elseif(NOT ANALYSIS_CLANG_PROVIDER STREQUAL "disabled")
    message(FATAL_ERROR "Unknown ANALYSIS_CLANG_PROVIDER=${ANALYSIS_CLANG_PROVIDER}")
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(history-core-tests tests/core_tests.cpp)
    target_link_libraries(history-core-tests PRIVATE history-core)
    target_compile_definitions(history-core-tests PRIVATE
        TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    add_test(NAME history-core-tests COMMAND history-core-tests)

    if(TARGET clang-extractor)
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
                -DANALYSIS_TOOL=$<TARGET_FILE:analysis-tool>
                -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
                -DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/extractor_integration.cmake)
    endif()
endif()

install(TARGETS history-core analysis-tool
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib)
if(TARGET clang-extractor)
    install(TARGETS clang-extractor RUNTIME DESTINATION bin)
endif()
