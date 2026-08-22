set(repository "${BINARY_DIR}/history-plan-repository")
set(plan "${BINARY_DIR}/history-plan.jsonl")
set(pr_facts "${BINARY_DIR}/history-plan-pr-facts.jsonl")
set(request "${BINARY_DIR}/history-plan-request.json")
file(REMOVE_RECURSE "${repository}")
file(MAKE_DIRECTORY "${repository}/src")

function(run_git)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${repository}" ${ARGN}
        RESULT_VARIABLE result ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "git ${ARGN} failed: ${error}")
    endif()
endfunction()

run_git(init -b main)
run_git(config user.name "History Planner Test")
run_git(config user.email "history-planner@example.invalid")
file(WRITE "${repository}/src/first.cpp" "int first() { return 1; }\n")
file(WRITE "${repository}/Makefile" "all:\n\t@echo test\n")
run_git(add .)
run_git(commit -m "Initial source")
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${repository}" rev-parse HEAD
    OUTPUT_VARIABLE first OUTPUT_STRIP_TRAILING_WHITESPACE)

run_git(mv src/first.cpp src/renamed.cpp)
run_git(commit -m "Rename source")
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${repository}" rev-parse HEAD
    OUTPUT_VARIABLE second OUTPUT_STRIP_TRAILING_WHITESPACE)

file(WRITE "${repository}/README.md" "history planner fixture\n")
run_git(add README.md)
run_git(commit -m "Document source")
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${repository}" rev-parse HEAD
    OUTPUT_VARIABLE third OUTPUT_STRIP_TRAILING_WHITESPACE)

file(WRITE "${repository}/DIRECT.txt" "not associated with a PR\n")
run_git(add DIRECT.txt)
run_git(commit -m "Direct maintenance commit")

file(WRITE "${pr_facts}"
    "{\"commit\":\"${first}\",\"pr_mapping_status\":\"no_pr\","
    "\"source\":\"bitbucket_data_center_api\"}\n"
    "{\"pr_id\":42,\"title\":\"Rename and document source\","
    "\"state\":\"MERGED\",\"source\":\"bitbucket_data_center_api\","
    "\"result_commit\":\"${third}\",\"associated_commits\":[\"${second}\"]}\n")
file(TO_CMAKE_PATH "${repository}" repository_json)
file(TO_CMAKE_PATH "${plan}" plan_json)
file(TO_CMAKE_PATH "${pr_facts}" pr_facts_json)
file(WRITE "${request}"
    "{\"schema_version\":1,\"query\":\"history.plan\",\"params\":{"
    "\"repository\":\"${repository_json}\",\"ref\":\"main\","
    "\"output\":\"${plan_json}\",\"pr_facts\":\"${pr_facts_json}\"}}")

execute_process(COMMAND "${REPOTRAVERSE}" query --request "${request}"
    OUTPUT_VARIABLE response RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "history.plan query failed: ${response}")
endif()
foreach(expected
        "\"commit_count\":4"
        "\"change_unit_count\":3"
        "\"pr_identified_commits\":2"
        "\"pr_no_pr_commits\":1"
        "\"pr_unknown_commits\":1")
    string(FIND "${response}" "${expected}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "missing ${expected}: ${response}")
    endif()
endforeach()

set(partial_plan "${BINARY_DIR}/history-plan-partial.jsonl")
file(TO_CMAKE_PATH "${partial_plan}" partial_plan_json)
file(WRITE "${request}"
    "{\"schema_version\":1,\"query\":\"history.plan\",\"params\":{"
    "\"repository\":\"${repository_json}\",\"ref\":\"main\","
    "\"start_exclusive\":\"${first}\",\"output\":\"${partial_plan_json}\","
    "\"pr_facts\":\"${pr_facts_json}\"}}")
execute_process(COMMAND "${REPOTRAVERSE}" query --request "${request}"
    OUTPUT_VARIABLE response RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "partial history.plan query failed: ${response}")
endif()
foreach(expected "\"commit_count\":3" "\"change_unit_count\":2")
    string(FIND "${response}" "${expected}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "missing ${expected} from partial plan: ${response}")
    endif()
endforeach()

file(READ "${plan}" plan_contents)
foreach(expected
        "\"change_unit_id\":\"bitbucket-pr:42\""
        "\"grouping_status\":\"no_pr\""
        "\"base_commit\":\"${first}\""
        "\"head_commit\":\"${third}\""
        "\"old_path\":\"src/first.cpp\""
        "\"path\":\"src/renamed.cpp\""
        "\"status\":\"R100\"")
    string(FIND "${plan_contents}" "${expected}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "missing ${expected} in history plan")
    endif()
endforeach()
