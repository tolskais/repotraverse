include_guard(GLOBAL)

set(REPOTRAVERSE_CURL_ROOT "" CACHE PATH
    "Root of the pinned libcurl 8.21.0 SDK")

if(REPOTRAVERSE_CURL_ROOT)
    cmake_path(ABSOLUTE_PATH REPOTRAVERSE_CURL_ROOT NORMALIZE)
    find_package(CURL 8.21.0 EXACT CONFIG REQUIRED
        PATHS "${REPOTRAVERSE_CURL_ROOT}/lib/cmake/CURL"
              "${REPOTRAVERSE_CURL_ROOT}/lib64/cmake/CURL"
        NO_DEFAULT_PATH)
else()
    # Linux development builds may use a separately provisioned pinned SDK.
    # Configuration still never downloads dependencies.
    find_package(CURL 8.21.0 EXACT REQUIRED)
endif()

if(NOT TARGET CURL::libcurl)
    message(FATAL_ERROR "The libcurl SDK must export CURL::libcurl")
endif()
