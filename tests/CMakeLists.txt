find_package(Git REQUIRED)

# Catch2 v3.15.0 official amalgamated distribution keeps the test build
# offline without importing Catch2's own development targets.
add_library(repotraverse-catch2 STATIC
    "${PROJECT_SOURCE_DIR}/third_party/Catch2/extras/catch_amalgamated.cpp")
target_include_directories(repotraverse-catch2 SYSTEM PUBLIC
    "${PROJECT_SOURCE_DIR}/third_party/Catch2/extras")
add_library(Catch2::Catch2WithMain ALIAS repotraverse-catch2)

function(repotraverse_add_test target source)
    set(options CATCH)
    set(multi_value_args DEFINITIONS DEPENDENCIES LIBRARIES)
    cmake_parse_arguments(TEST "${options}" "" "${multi_value_args}" ${ARGN})

    add_executable(${target} "${CMAKE_CURRENT_SOURCE_DIR}/${source}")
    target_link_libraries(${target} PRIVATE
        repotraverse-build-options ${TEST_LIBRARIES})
    if(TEST_CATCH)
        target_link_libraries(${target} PRIVATE Catch2::Catch2WithMain)
    endif()
    if(TEST_DEFINITIONS)
        target_compile_definitions(${target} PRIVATE ${TEST_DEFINITIONS})
    endif()
    if(TEST_DEPENDENCIES)
        add_dependencies(${target} ${TEST_DEPENDENCIES})
    endif()
    add_test(NAME ${target} COMMAND ${target})
endfunction()

repotraverse_add_test(history-core-tests core_tests.cpp
    LIBRARIES history-core
    DEFINITIONS TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
repotraverse_add_test(git-coordination-tests git_coordination_tests.cpp
    LIBRARIES history-core)
repotraverse_add_test(file-history-tests file_history_tests.cpp
    LIBRARIES history-core)
repotraverse_add_test(build-import-tests build_import_tests.cpp
    LIBRARIES history-core
    DEFINITIONS TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
repotraverse_add_test(http-roundtrip-tests http_roundtrip_tests.cpp
    CATCH LIBRARIES history-core)
repotraverse_add_test(catalog-production-tests catalog_production_tests.cpp
    CATCH LIBRARIES history-core)
repotraverse_add_test(process-tests process_tests.cpp LIBRARIES history-core)
repotraverse_add_test(encoding-tests encoding_tests.cpp
    LIBRARIES history-core
    DEFINITIONS PROBE_PATH="$<TARGET_FILE:repotraverse-compiler-probe>")
repotraverse_add_test(stability-tests stability_tests.cpp
    CATCH LIBRARIES history-core)
repotraverse_add_test(revision-workspace-tests revision_workspace_tests.cpp
    LIBRARIES history-core
    DEFINITIONS GIT_PATH="${GIT_EXECUTABLE}")
repotraverse_add_test(progressive-tests progressive_tests.cpp
    LIBRARIES history-core
    DEFINITIONS GIT_PATH="${GIT_EXECUTABLE}")
repotraverse_add_test(change-evidence-tests change_evidence_tests.cpp
    LIBRARIES history-core
    DEFINITIONS GIT_PATH="${GIT_EXECUTABLE}")

add_test(NAME compiler-probe-version
    COMMAND repotraverse-compiler-probe --version)
set_tests_properties(compiler-probe-version PROPERTIES
    PASS_REGULAR_EXPRESSION
        "repotraverse compiler-probe ${PROJECT_VERSION} artifact v1")
add_test(NAME repotraverse-version COMMAND repotraverse --version)
set_tests_properties(repotraverse-version PROPERTIES
    PASS_REGULAR_EXPRESSION
        "repotraverse ${PROJECT_VERSION}.*build mode: ${REPOTRAVERSE_BUILD_MODE}")
add_test(NAME repotraverse-help COMMAND repotraverse --help)
set_tests_properties(repotraverse-help PROPERTIES
    PASS_REGULAR_EXPRESSION "experiment.*serve.*query")
add_test(
    NAME public-version-v1
    COMMAND ${CMAKE_COMMAND}
        -DREPOTRAVERSE=$<TARGET_FILE:repotraverse>
        -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/version_consistency.cmake)
add_test(
    NAME history-plan-integration
    COMMAND ${CMAKE_COMMAND}
        -DREPOTRAVERSE=$<TARGET_FILE:repotraverse>
        -DGIT_EXECUTABLE=${GIT_EXECUTABLE}
        -DBINARY_DIR=${PROJECT_BINARY_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/history_plan_integration.cmake)

add_executable(fake-make fake_make.cpp)
target_link_libraries(fake-make PRIVATE history-core repotraverse-build-options)
add_executable(fake-arm-compiler fake_arm_compiler.cpp)
target_link_libraries(fake-arm-compiler PRIVATE repotraverse-build-options)

repotraverse_add_test(experiment-tests experiment_tests.cpp
    LIBRARIES history-core
    DEFINITIONS
        EXTRACTOR_PATH="$<TARGET_FILE:clang-extractor>"
        PROBE_PATH="$<TARGET_FILE:repotraverse-compiler-probe>"
        FAKE_MAKE_PATH="$<TARGET_FILE:fake-make>"
        FAKE_ARM_COMPILER_PATH="$<TARGET_FILE:fake-arm-compiler>"
        GIT_PATH="${GIT_EXECUTABLE}"
    DEPENDENCIES
        clang-extractor repotraverse-compiler-probe fake-make fake-arm-compiler)
repotraverse_add_test(worker-integration-tests worker_integration.cpp
    LIBRARIES history-core
    DEFINITIONS EXTRACTOR_PATH="$<TARGET_FILE:clang-extractor>"
    DEPENDENCIES clang-extractor)

add_test(NAME clang-extractor-version COMMAND clang-extractor --version)
set_tests_properties(clang-extractor-version PROPERTIES
    PASS_REGULAR_EXPRESSION
        "repotraverse clang-extractor ${PROJECT_VERSION}.*LLVM/Clang")
add_test(
    NAME clang-extractor-smoke
    COMMAND clang-extractor "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/sample.cpp"
        -- -std=c++17)
set_tests_properties(clang-extractor-smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "qualified_name")
add_test(
    NAME clang-extractor-lineage
    COMMAND ${CMAKE_COMMAND}
        -DEXTRACTOR=$<TARGET_FILE:clang-extractor>
        -DREPOTRAVERSE=$<TARGET_FILE:repotraverse>
        -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
        -DBINARY_DIR=${PROJECT_BINARY_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/extractor_integration.cmake)
repotraverse_add_test(extractor-entities-tests extractor_entities_tests.cpp
    LIBRARIES history-core
    DEFINITIONS
        EXTRACTOR_PATH="$<TARGET_FILE:clang-extractor>"
        TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    DEPENDENCIES clang-extractor)
