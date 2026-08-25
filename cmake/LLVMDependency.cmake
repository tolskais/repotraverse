include_guard(GLOBAL)

if(NOT CMAKE_C_COMPILER_ID STREQUAL "Clang" OR
   NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR
        "Repotraverse requires Clang for both C and C++; selected compilers: "
        "${CMAKE_C_COMPILER_ID} and ${CMAKE_CXX_COMPILER_ID}")
endif()

function(repotraverse_detect_llvm_msvc_runtime output)
    set(_runtime "${CMAKE_MSVC_RUNTIME_LIBRARY}")
    if(NOT _runtime)
        set(_probe_library "${REPOTRAVERSE_LLVM_ROOT}/lib/clangTooling.lib")
        find_program(_llvm_readobj
            NAMES llvm-readobj.exe llvm-readobj
            PATHS "${REPOTRAVERSE_LLVM_ROOT}/bin"
            NO_DEFAULT_PATH)
        if(NOT _llvm_readobj OR NOT EXISTS "${_probe_library}")
            message(FATAL_ERROR
                "The LLVM SDK does not export CMAKE_MSVC_RUNTIME_LIBRARY and "
                "its runtime policy cannot be inspected")
        endif()
        execute_process(
            COMMAND "${_llvm_readobj}" --coff-directives "${_probe_library}"
            RESULT_VARIABLE _readobj_result
            OUTPUT_VARIABLE _directives
            ERROR_VARIABLE _readobj_error)
        if(NOT _readobj_result EQUAL 0)
            message(FATAL_ERROR
                "Cannot inspect the LLVM SDK runtime policy: ${_readobj_error}")
        endif()
        string(REGEX MATCHALL "RuntimeLibrary=[A-Za-z_]+"
            _runtime_directives "${_directives}")
        list(REMOVE_DUPLICATES _runtime_directives)
        list(LENGTH _runtime_directives _runtime_directive_count)
        if(NOT _runtime_directive_count EQUAL 1)
            message(FATAL_ERROR
                "The LLVM SDK has no single MSVC runtime policy: "
                "${_runtime_directives}")
        endif()
        list(GET _runtime_directives 0 _runtime_directive)
        if(_runtime_directive STREQUAL "RuntimeLibrary=MT_StaticRelease")
            set(_runtime "MultiThreaded")
        elseif(_runtime_directive STREQUAL "RuntimeLibrary=MTd_StaticDebug")
            set(_runtime "MultiThreadedDebug")
        elseif(_runtime_directive STREQUAL "RuntimeLibrary=MD_DynamicRelease")
            set(_runtime "MultiThreadedDLL")
        elseif(_runtime_directive STREQUAL "RuntimeLibrary=MDd_DynamicDebug")
            set(_runtime "MultiThreadedDebugDLL")
        else()
            message(FATAL_ERROR
                "Unsupported LLVM SDK runtime policy: ${_runtime_directive}")
        endif()
    endif()
    set(${output} "${_runtime}" PARENT_SCOPE)
endfunction()

# LLVM/Clang is a product dependency, so import its policy before creating any
# Repotraverse target. An explicit root keeps Windows and offline builds pinned;
# otherwise the normal Clang package lookup locates its matching LLVM package.
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
    find_package(Clang CONFIG REQUIRED)
endif()

if(MSVC)
    if(NOT REPOTRAVERSE_LLVM_ROOT)
        message(FATAL_ERROR
            "Windows Clang builds require REPOTRAVERSE_LLVM_ROOT")
    endif()
    repotraverse_detect_llvm_msvc_runtime(_repotraverse_llvm_msvc_runtime)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "${_repotraverse_llvm_msvc_runtime}")
    message(STATUS
        "Repotraverse MSVC runtime follows LLVM: "
        "${_repotraverse_llvm_msvc_runtime}")
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
message(STATUS "Repotraverse Clang linkage: ${_repotraverse_clang_linkage}")

if(WIN32)
    set(_repotraverse_clang_resource_include
        "${REPOTRAVERSE_LLVM_ROOT}/lib/clang/${LLVM_VERSION_MAJOR}/include")
    set(_repotraverse_llvm_license "${REPOTRAVERSE_LLVM_ROOT}/LICENSE.TXT")
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
