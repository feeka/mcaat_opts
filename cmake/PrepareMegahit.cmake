# Idempotent source patching; never reset or overwrite unrelated submodule edits.
function(mcaat_prepare_megahit)
    set(megahit_dir "${PROJECT_SOURCE_DIR}/libs/megahit")
    set(header "${megahit_dir}/src/sdbg/sdbg.h")
    set(patch "${PROJECT_SOURCE_DIR}/patches/megahit-cycle-multiplicity.patch")
    if(NOT EXISTS "${header}")
        message(FATAL_ERROR
            "MEGAHIT is missing. Run git submodule update --init --recursive, "
            "then configure again.")
    endif()
    file(READ "${header}" header_text)
    string(FIND "${header_text}" "bool HasFullMultiplicityStorage() const" layout_pos)
    string(FIND "${header_text}" "bool CycleMultiplicityAtLeast(" predicate_pos)
    if(NOT layout_pos EQUAL -1 AND NOT predicate_pos EQUAL -1)
        return()
    endif()

    find_package(Git REQUIRED)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" apply --check "${patch}"
        WORKING_DIRECTORY "${megahit_dir}"
        RESULT_VARIABLE check_result
        OUTPUT_VARIABLE check_output ERROR_VARIABLE check_error)
    if(NOT check_result EQUAL 0)
        message(FATAL_ERROR
            "MEGAHIT lacks the required cycle-search API, and its local header "
            "does not accept the bundled patch. No files were changed. "
            "Resolve the submodule version/local edits and configure again.\n"
            "${check_output}${check_error}")
    endif()
    message(STATUS "Applying bundled MEGAHIT cycle-multiplicity accessor patch")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" apply "${patch}"
        WORKING_DIRECTORY "${megahit_dir}"
        RESULT_VARIABLE apply_result
        OUTPUT_VARIABLE apply_output ERROR_VARIABLE apply_error)
    if(NOT apply_result EQUAL 0)
        message(FATAL_ERROR "MEGAHIT patch failed:\n${apply_output}${apply_error}")
    endif()
endfunction()
