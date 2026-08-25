include_guard(GLOBAL)

if(WIN32 AND NOT MSVC)
    message(FATAL_ERROR
        "Windows builds require clang-cl from REPOTRAVERSE_LLVM_ROOT; "
        "the selected Clang compiler does not use the MSVC command-line ABI")
endif()
if(WIN32 AND NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Repotraverse supports 64-bit Windows only")
endif()

set(REPOTRAVERSE_WINDOWS_MANIFEST
    "${PROJECT_SOURCE_DIR}/resources/windows/repotraverse.manifest")

function(repotraverse_add_windows_manifests)
    if(NOT WIN32)
        return()
    endif()
    foreach(target IN LISTS ARGN)
        # Let link.exe merge longPathAware with its generated CRT manifest.
        # Embedding an RT_MANIFEST resource through an RC file duplicates ID 1.
        target_link_options(${target} PRIVATE
            "/MANIFEST:EMBED"
            "/MANIFESTINPUT:${REPOTRAVERSE_WINDOWS_MANIFEST}")
    endforeach()
endfunction()
