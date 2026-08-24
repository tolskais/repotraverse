cmake_minimum_required(VERSION 3.24)

project(repotraverse VERSION 1.0.0 LANGUAGES C CXX)

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
if(WIN32 AND NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Repotraverse supports 64-bit Windows only")
endif()

if(WIN32)
    enable_language(RC)
    set(REPOTRAVERSE_WINDOWS_MANIFEST
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/windows/repotraverse.manifest")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/windows/repotraverse.rc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/repotraverse.rc"
        @ONLY)
endif()

function(repotraverse_add_windows_manifest target)
    if(WIN32)
        target_sources(${target} PRIVATE
            "${CMAKE_CURRENT_BINARY_DIR}/repotraverse.rc")
    endif()
endfunction()

if(REPOTRAVERSE_NATIVE_OPTIMIZATION)
    set(REPOTRAVERSE_BUILD_MODE "native")
else()
    set(REPOTRAVERSE_BUILD_MODE "generic")
endif()

add_library(repotraverse-build-options INTERFACE)
if(MSVC)
    target_compile_options(repotraverse-build-options INTERFACE
        $<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
endif()
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

# Pinned offline dependencies. SQLite is the disposable local materialized
# index; xxHash supplies schema-fixed XXH3-128 identifiers.
find_package(Threads REQUIRED)

add_library(repotraverse-xxhash STATIC third_party/xxhash/xxhash.c)
target_include_directories(repotraverse-xxhash SYSTEM PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/xxhash")
set_target_properties(repotraverse-xxhash PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(repotraverse-sqlite STATIC third_party/sqlite/sqlite3.c)
target_include_directories(repotraverse-sqlite SYSTEM PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite")
target_compile_definitions(repotraverse-sqlite PRIVATE
    SQLITE_DQS=0
    SQLITE_DEFAULT_MEMSTATUS=0
    SQLITE_OMIT_DEPRECATED
    SQLITE_THREADSAFE=1)
if(NOT WIN32)
    target_link_libraries(repotraverse-sqlite PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)
endif()

# Pinned Tree-sitter v0.26.11 with the generated C v0.24.2 and C++ v0.23.4
# parsers. Only the embeddable runtime and generated parser sources are built;
# configuration never invokes the Tree-sitter CLI or accesses the network.
add_library(repotraverse-tree-sitter STATIC
    third_party/tree_sitter/lib/src/lib.c
    third_party/tree_sitter_c/src/parser.c
    third_party/tree_sitter_cpp/src/parser.c
    third_party/tree_sitter_cpp/src/scanner.c)
target_include_directories(repotraverse-tree-sitter SYSTEM PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tree_sitter/lib/include")
target_include_directories(repotraverse-tree-sitter SYSTEM PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tree_sitter/lib/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tree_sitter_c/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tree_sitter_cpp/src")
set_target_properties(repotraverse-tree-sitter PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    C_STANDARD 11
    C_STANDARD_REQUIRED ON)

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_info.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/history/build_info.hpp"
    @ONLY)

add_library(history-core
    src/build_import.cpp
    src/catalog.cpp
    src/change_evidence.cpp
    src/config.cpp
    src/encoding.cpp
    src/experiment.cpp
    src/file_history.cpp
    src/git_coordination.cpp
    src/history_plan.cpp
    src/http.cpp
    src/ir.cpp
    src/lineage.cpp
    src/process.cpp
    src/progressive.cpp
    src/query.cpp)
target_sources(history-core PRIVATE src/worker.cpp src/telemetry.cpp src/stability.cpp
    src/revision_workspace.cpp)
target_include_directories(history-core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(history-core
    PUBLIC nlohmann_json::nlohmann_json
    PRIVATE repotraverse-sqlite repotraverse-xxhash repotraverse-tree-sitter
        Threads::Threads
    PRIVATE repotraverse-build-options)
if(WIN32)
    target_link_libraries(history-core PRIVATE ws2_32 winhttp)
endif()
target_compile_features(history-core PUBLIC cxx_std_20)

add_executable(repotraverse app/main.cpp)
target_link_libraries(repotraverse PRIVATE history-core repotraverse-build-options)

add_executable(repotraverse-compiler-probe probe/main.cpp)
target_link_libraries(repotraverse-compiler-probe PRIVATE
    history-core repotraverse-build-options)

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

    if(WIN32 AND TARGET clangTooling AND TARGET clangIndex)
        set(_repotraverse_clang_libraries clangTooling clangIndex)
        set(_repotraverse_clang_linkage "component")
    elseif(TARGET clang-cpp AND TARGET LLVM)
        set(_repotraverse_clang_libraries clang-cpp LLVM)
        set(_repotraverse_clang_linkage "monolithic")
    elseif(TARGET clangTooling AND TARGET clangIndex)
        set(_repotraverse_clang_libraries clangTooling clangIndex)
        set(_repotraverse_clang_linkage "component")
    else()
        message(FATAL_ERROR
            "The LLVM/Clang SDK must export either clangTooling and clangIndex "
            "or the monolithic clang-cpp and LLVM CMake targets")
    endif()
    message(STATUS
        "Repotraverse Clang linkage: ${_repotraverse_clang_linkage}")

    if(WIN32)
        if(NOT REPOTRAVERSE_LLVM_ROOT)
            message(FATAL_ERROR
                "Windows Clang builds require REPOTRAVERSE_LLVM_ROOT")
        endif()
        set(_repotraverse_clang_resource_include
            "${REPOTRAVERSE_LLVM_ROOT}/lib/clang/${LLVM_VERSION_MAJOR}/include")
        set(_repotraverse_llvm_license
            "${REPOTRAVERSE_LLVM_ROOT}/LICENSE.TXT")
        if(NOT IS_DIRECTORY "${_repotraverse_clang_resource_include}")
            message(FATAL_ERROR
                "The LLVM SDK does not contain Clang ${LLVM_VERSION_MAJOR} "
                "resource headers: ${_repotraverse_clang_resource_include}")
        endif()
        if(NOT EXISTS "${_repotraverse_llvm_license}")
            message(FATAL_ERROR
                "The LLVM SDK does not contain LICENSE.TXT: "
                "${REPOTRAVERSE_LLVM_ROOT}")
        endif()
    endif()

    add_executable(clang-extractor extractor/main.cpp)
    target_include_directories(clang-extractor PRIVATE
        ${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})
    separate_arguments(_llvm_definitions NATIVE_COMMAND "${LLVM_DEFINITIONS}")
    target_compile_options(clang-extractor PRIVATE ${_llvm_definitions})
    target_link_libraries(clang-extractor PRIVATE
        history-core repotraverse-build-options
        ${_repotraverse_clang_libraries})
elseif(NOT REPOTRAVERSE_CLANG_PROVIDER STREQUAL "disabled")
    message(FATAL_ERROR
        "Unknown REPOTRAVERSE_CLANG_PROVIDER=${REPOTRAVERSE_CLANG_PROVIDER}")
endif()

include(CTest)
if(BUILD_TESTING)
    find_package(Git REQUIRED)
    add_executable(history-core-tests tests/core_tests.cpp)
    target_link_libraries(history-core-tests PRIVATE
        history-core repotraverse-build-options)
    target_compile_definitions(history-core-tests PRIVATE
        TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    add_test(NAME history-core-tests COMMAND history-core-tests)
    add_executable(git-coordination-tests tests/git_coordination_tests.cpp)
    target_link_libraries(git-coordination-tests PRIVATE
        history-core repotraverse-build-options)
    add_test(NAME git-coordination-tests COMMAND git-coordination-tests)
    add_executable(file-history-tests tests/file_history_tests.cpp)
    target_link_libraries(file-history-tests PRIVATE history-core repotraverse-build-options)
    add_test(NAME file-history-tests COMMAND file-history-tests)
    add_executable(build-import-tests tests/build_import_tests.cpp)
    target_link_libraries(build-import-tests PRIVATE
        history-core repotraverse-build-options)
    target_compile_definitions(build-import-tests PRIVATE
        TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    add_test(NAME build-import-tests COMMAND build-import-tests)
    add_executable(http-roundtrip-tests tests/http_roundtrip_tests.cpp)
    target_link_libraries(http-roundtrip-tests PRIVATE
        history-core repotraverse-build-options)
    add_test(NAME http-roundtrip-tests COMMAND http-roundtrip-tests)
    add_executable(catalog-production-tests tests/catalog_production_tests.cpp)
    target_link_libraries(catalog-production-tests PRIVATE
        history-core repotraverse-build-options)
    add_test(NAME catalog-production-tests COMMAND catalog-production-tests)
    add_executable(process-tests tests/process_tests.cpp)
    target_link_libraries(process-tests PRIVATE
        history-core repotraverse-build-options)
    add_test(NAME process-tests COMMAND process-tests)
    add_executable(encoding-tests tests/encoding_tests.cpp)
    target_link_libraries(encoding-tests PRIVATE
        history-core repotraverse-build-options)
    target_compile_definitions(encoding-tests PRIVATE
        PROBE_PATH="$<TARGET_FILE:repotraverse-compiler-probe>")
    add_test(NAME encoding-tests COMMAND encoding-tests)
    add_executable(stability-tests tests/stability_tests.cpp)
    target_link_libraries(stability-tests PRIVATE
        history-core repotraverse-build-options)
    add_test(NAME stability-tests COMMAND stability-tests)
    add_executable(revision-workspace-tests tests/revision_workspace_tests.cpp)
    target_link_libraries(revision-workspace-tests PRIVATE
        history-core repotraverse-build-options)
    target_compile_definitions(revision-workspace-tests PRIVATE
        GIT_PATH="${GIT_EXECUTABLE}")
    add_test(NAME revision-workspace-tests COMMAND revision-workspace-tests)
    add_executable(progressive-tests tests/progressive_tests.cpp)
    target_link_libraries(progressive-tests PRIVATE
        history-core repotraverse-build-options)
    target_compile_definitions(progressive-tests PRIVATE
        GIT_PATH="${GIT_EXECUTABLE}")
    add_test(NAME progressive-tests COMMAND progressive-tests)
    add_executable(change-evidence-tests tests/change_evidence_tests.cpp)
    target_link_libraries(change-evidence-tests PRIVATE
        history-core repotraverse-build-options)
    target_compile_definitions(change-evidence-tests PRIVATE
        GIT_PATH="${GIT_EXECUTABLE}")
    add_test(NAME change-evidence-tests COMMAND change-evidence-tests)
    add_test(NAME compiler-probe-version
        COMMAND repotraverse-compiler-probe --version)
    set_tests_properties(compiler-probe-version PROPERTIES
        PASS_REGULAR_EXPRESSION
            "repotraverse compiler-probe ${PROJECT_VERSION} artifact v1")
    add_test(NAME repotraverse-version COMMAND repotraverse --version)
    set_tests_properties(repotraverse-version PROPERTIES
        PASS_REGULAR_EXPRESSION
            "repotraverse ${PROJECT_VERSION}.*build mode: ${REPOTRAVERSE_BUILD_MODE}")
    add_test(
        NAME public-version-v1
        COMMAND ${CMAKE_COMMAND}
            -DREPOTRAVERSE=$<TARGET_FILE:repotraverse>
            -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/version_consistency.cmake)
    add_test(
        NAME history-plan-integration
        COMMAND ${CMAKE_COMMAND}
            -DREPOTRAVERSE=$<TARGET_FILE:repotraverse>
            -DGIT_EXECUTABLE=${GIT_EXECUTABLE}
            -DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/history_plan_integration.cmake)

    if(TARGET clang-extractor)
        add_executable(fake-make tests/fake_make.cpp)
        target_link_libraries(fake-make PRIVATE history-core repotraverse-build-options)
        add_executable(fake-arm-compiler tests/fake_arm_compiler.cpp)
        add_executable(experiment-tests tests/experiment_tests.cpp)
        target_link_libraries(experiment-tests PRIVATE
            history-core repotraverse-build-options)
        target_compile_definitions(experiment-tests PRIVATE
            EXTRACTOR_PATH="$<TARGET_FILE:clang-extractor>"
            PROBE_PATH="$<TARGET_FILE:repotraverse-compiler-probe>"
            FAKE_MAKE_PATH="$<TARGET_FILE:fake-make>"
            FAKE_ARM_COMPILER_PATH="$<TARGET_FILE:fake-arm-compiler>"
            GIT_PATH="${GIT_EXECUTABLE}")
        add_dependencies(experiment-tests clang-extractor
            repotraverse-compiler-probe fake-make fake-arm-compiler)
        add_test(NAME experiment-tests COMMAND experiment-tests)
        add_executable(worker-integration-tests tests/worker_integration.cpp)
        target_link_libraries(worker-integration-tests PRIVATE
            history-core repotraverse-build-options)
        target_compile_definitions(worker-integration-tests PRIVATE
            EXTRACTOR_PATH="$<TARGET_FILE:clang-extractor>")
        add_dependencies(worker-integration-tests clang-extractor)
        add_test(NAME worker-integration-tests COMMAND worker-integration-tests)
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
        add_executable(extractor-entities-tests tests/extractor_entities_tests.cpp)
        target_link_libraries(extractor-entities-tests PRIVATE
            history-core repotraverse-build-options)
        target_compile_definitions(extractor-entities-tests PRIVATE
            EXTRACTOR_PATH="$<TARGET_FILE:clang-extractor>"
            TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
        add_dependencies(extractor-entities-tests clang-extractor)
        add_test(NAME extractor-entities-tests COMMAND extractor-entities-tests)
    endif()
endif()

install(TARGETS history-core repotraverse repotraverse-compiler-probe
    repotraverse-sqlite repotraverse-xxhash repotraverse-tree-sitter
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib)
if(TARGET clang-extractor)
    install(TARGETS clang-extractor RUNTIME DESTINATION bin)
    if(WIN32)
        install(DIRECTORY "${_repotraverse_clang_resource_include}/"
            DESTINATION "lib/clang/${LLVM_VERSION_MAJOR}/include")
        install(FILES "${_repotraverse_llvm_license}"
            DESTINATION licenses/llvm)
    endif()
endif()
install(FILES third_party/nlohmann_json/LICENSE.MIT
    DESTINATION licenses/nlohmann_json)
install(FILES third_party/xxhash/LICENSE
    DESTINATION licenses/xxhash)
install(FILES third_party/sqlite/README.md
    DESTINATION licenses/sqlite)
install(FILES third_party/tree_sitter/LICENSE third_party/tree_sitter/VERSION
    DESTINATION licenses/tree_sitter)
install(FILES third_party/tree_sitter_c/LICENSE third_party/tree_sitter_c/VERSION
    DESTINATION licenses/tree_sitter_c)
install(FILES third_party/tree_sitter_cpp/LICENSE
    third_party/tree_sitter_cpp/VERSION
    DESTINATION licenses/tree_sitter_cpp)
install(FILES README.md THIRD_PARTY.md docs/lineage-architecture.md
    docs/operations.md docs/v1-experiment.md
    DESTINATION docs)
install(FILES LICENSE DESTINATION .)
install(FILES config/service.example.json config/experiment.example.json
    DESTINATION config)
if(WIN32)
    install(FILES tools/install-service.ps1 tools/uninstall-service.ps1
        tools/package-windows.ps1
        DESTINATION tools)

    # Every executable that exercises history-core must opt in independently;
    # a manifest on the parent process does not affect child executables.
    get_property(_repotraverse_directory_targets DIRECTORY
        PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_repotraverse_target IN LISTS _repotraverse_directory_targets)
        get_target_property(_repotraverse_target_type
            ${_repotraverse_target} TYPE)
        if(_repotraverse_target_type STREQUAL "EXECUTABLE")
            repotraverse_add_windows_manifest(${_repotraverse_target})
        endif()
    endforeach()
endif()
