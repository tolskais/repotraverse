cmake_minimum_required(VERSION 3.24)

project(repotraverse VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(REPOTRAVERSE_LLVM_ROOT "" CACHE PATH
    "Root of the prebuilt LLVM/Clang SDK")
option(REPOTRAVERSE_NATIVE_OPTIMIZATION
       "Optimize for the build VM CPU; the resulting executable is not portable"
       OFF)

include(cmake/Windows.cmake)
include(cmake/LLVMDependency.cmake)

if(REPOTRAVERSE_NATIVE_OPTIMIZATION)
    set(REPOTRAVERSE_BUILD_MODE "native")
else()
    set(REPOTRAVERSE_BUILD_MODE "generic")
endif()

add_library(repotraverse-build-options INTERFACE)
if(MSVC)
    target_compile_options(repotraverse-build-options INTERFACE
        $<$<COMPILE_LANGUAGE:CXX>:/utf-8>
        $<$<COMPILE_LANGUAGE:CXX>:/W4>
        $<$<COMPILE_LANGUAGE:CXX>:/WX>)
else()
    target_compile_options(repotraverse-build-options INTERFACE
        $<$<COMPILE_LANGUAGE:CXX>:-Wall>
        $<$<COMPILE_LANGUAGE:CXX>:-Wextra>
        $<$<COMPILE_LANGUAGE:CXX>:-Wpedantic>
        $<$<COMPILE_LANGUAGE:CXX>:-Werror>)
endif()
if(REPOTRAVERSE_NATIVE_OPTIMIZATION)
    if(MSVC)
        target_compile_options(repotraverse-build-options INTERFACE
            $<$<COMPILE_LANGUAGE:CXX>:/clang:-march=native>)
    else()
        target_compile_options(repotraverse-build-options INTERFACE
            $<$<COMPILE_LANGUAGE:CXX>:-march=native>
            $<$<COMPILE_LANGUAGE:CXX>:-mtune=native>)
    endif()
endif()

# Pinned nlohmann/json v3.12.0 is vendored so configuration never consults a
# package manager or the network.
add_library(repotraverse-json INTERFACE)
add_library(nlohmann_json::nlohmann_json ALIAS repotraverse-json)
target_include_directories(repotraverse-json SYSTEM INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann_json/include")

# Pinned CLI11 v2.7.0 single-header distribution. It is vendored so command
# parsing remains available in fully offline builds.
add_library(repotraverse-cli11 INTERFACE)
add_library(CLI11::CLI11 ALIAS repotraverse-cli11)
target_include_directories(repotraverse-cli11 SYSTEM INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/CLI11/include")

# Pinned offline dependencies. SQLite supplies the local transactional work
# queue and rebuildable indexes; xxHash supplies schema-fixed XXH3-128
# identifiers.
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
target_sources(history-core PRIVATE src/worker.cpp src/telemetry.cpp
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
target_link_libraries(repotraverse PRIVATE history-core repotraverse-build-options
    CLI11::CLI11)

add_executable(repotraverse-compiler-probe probe/main.cpp)
target_link_libraries(repotraverse-compiler-probe PRIVATE
    history-core repotraverse-build-options)

add_executable(clang-extractor extractor/main.cpp)
target_include_directories(clang-extractor SYSTEM PRIVATE
    ${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})
separate_arguments(_llvm_definitions NATIVE_COMMAND "${LLVM_DEFINITIONS}")
target_compile_options(clang-extractor PRIVATE ${_llvm_definitions})
# LLVM's imported targets do not expose their RTTI mode as an interface compile
# option.  A consumer of Clang's C++ APIs must nevertheless use the same mode:
# otherwise a no-RTTI SDK leaves references to LLVM/Clang typeinfo unresolved.
if(NOT LLVM_ENABLE_RTTI)
    if(MSVC)
        target_compile_options(clang-extractor PRIVATE /GR-)
    else()
        target_compile_options(clang-extractor PRIVATE -fno-rtti)
    endif()
endif()
target_link_libraries(clang-extractor PRIVATE
    history-core repotraverse-build-options
    ${_repotraverse_clang_libraries})

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

install(TARGETS repotraverse repotraverse-compiler-probe
    RUNTIME DESTINATION bin)
install(TARGETS clang-extractor RUNTIME DESTINATION bin)
if(WIN32)
    install(DIRECTORY "${_repotraverse_clang_resource_include}/"
        DESTINATION "lib/clang/${LLVM_VERSION_MAJOR}/include")
    install(FILES "${_repotraverse_llvm_license}"
        DESTINATION licenses/llvm)
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
install(FILES third_party/CLI11/LICENSE third_party/CLI11/VERSION
    DESTINATION licenses/CLI11)
install(FILES README.md THIRD_PARTY.md docs/lineage-architecture.md
    docs/operations.md docs/investigation-model.md docs/poc-runbook.md
    docs/v1-experiment.md
    DESTINATION docs)
install(FILES LICENSE DESTINATION .)
install(FILES config/service.example.json config/experiment.example.json
    DESTINATION config)
if(WIN32)
    install(FILES tools/install-service.ps1 tools/uninstall-service.ps1
        tools/package-windows.ps1
        DESTINATION tools)
endif()

# Manifests are a property of the shipped processes, not test runners and
# compiler fixtures created by this build.
repotraverse_add_windows_manifests(
    repotraverse repotraverse-compiler-probe clang-extractor)
