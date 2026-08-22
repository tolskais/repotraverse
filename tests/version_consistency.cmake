execute_process(
    COMMAND "${REPOTRAVERSE}" status
    RESULT_VARIABLE status_code
    OUTPUT_VARIABLE status_json
    ERROR_VARIABLE status_error)
if(NOT status_code EQUAL 0)
    message(FATAL_ERROR "status failed: ${status_error}")
endif()
string(JSON schema GET "${status_json}" schema_version)
string(JSON tool_version GET "${status_json}" tool_version)
if(NOT schema EQUAL 1 OR NOT tool_version STREQUAL "1.0.0")
    message(FATAL_ERROR "public tool/schema versions are not v1: ${status_json}")
endif()
foreach(field query_transport federated_service shared_transport
              identifier_model history_planner file_history
              build_context_adapter background_worker lineage_model)
    string(JSON value GET "${status_json}" "${field}")
    if(NOT value MATCHES "_v1$")
        message(FATAL_ERROR "${field} is not v1: ${value}")
    endif()
endforeach()
file(READ "${SOURCE_DIR}/tools/package-windows.ps1" package_script)
if(NOT package_script MATCHES "versionInfo = \"1.0.0\"")
    message(FATAL_ERROR "package metadata is not version 1.0.0")
endif()
if(package_script MATCHES "signtool|SigningCertificate")
    message(FATAL_ERROR "v1 packaging unexpectedly requires signing")
endif()
