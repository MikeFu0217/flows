include_guard(GLOBAL)

# Call after add_subdirectory(JUCE), before creating/linking Flows plugin targets.
# Only the generated copy is compiled. The user's JUCE checkout stays untouched.
function(flows_use_juce_adapters juce_root)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    get_filename_component(_juce_root "${juce_root}" REALPATH)
    set(_script "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../tools/patch_juce_wrappers.py")
    set(_generated "${CMAKE_BINARY_DIR}/generated/flows-juce-adapters")
    set(_original "${_juce_root}/modules/juce_audio_plugin_client")
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${_script}"
            --juce-root "${_juce_root}" --destination "${_generated}"
        RESULT_VARIABLE _status OUTPUT_VARIABLE _output ERROR_VARIABLE _error
    )
    if(NOT _status EQUAL 0)
        message(FATAL_ERROR "Flows requires reviewed JUCE 8.0.7 automation wrappers:\n${_output}${_error}")
    endif()
    string(STRIP "${_output}" _output)
    message(STATUS "${_output}")

    set(_patched_targets 0)
    foreach(_format IN ITEMS VST3 AU)
        set(_target "juce_audio_plugin_client_${_format}")
        if(NOT TARGET "${_target}")
            continue()
        endif()
        get_target_property(_sources "${_target}" INTERFACE_SOURCES)
        if(NOT _sources)
            message(FATAL_ERROR "${_target} has no INTERFACE_SOURCES; JUCE wrapper layout changed")
        endif()
        set(_replacement)
        foreach(_source IN LISTS _sources)
            string(FIND "${_source}" "${_original}/" _prefix)
            if(NOT _prefix EQUAL 0)
                message(FATAL_ERROR "Unexpected ${_target} source: ${_source}; refusing a partial adapter patch")
            endif()
            string(REPLACE "${_original}/" "${_generated}/juce_audio_plugin_client/" _new_source "${_source}")
            if(NOT EXISTS "${_new_source}")
                message(FATAL_ERROR "Generated JUCE wrapper source missing: ${_new_source}")
            endif()
            list(APPEND _replacement "${_new_source}")
        endforeach()
        set_property(TARGET "${_target}" PROPERTY INTERFACE_SOURCES "${_replacement}")
        target_include_directories("${_target}" BEFORE INTERFACE
            "${_generated}" "${_generated}/juce_audio_plugin_client/AU")
        math(EXPR _patched_targets "${_patched_targets} + 1")
    endforeach()
    if(_patched_targets EQUAL 0)
        message(FATAL_ERROR "Call flows_use_juce_adapters after JUCE has created its plugin wrapper targets")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${_script}" "${_juce_root}/modules/juce_core/system/juce_StandardHeader.h"
        "${_original}/juce_audio_plugin_client_VST3.cpp"
        "${_original}/juce_audio_plugin_client_AU_1.mm"
        "${_original}/AU/AudioUnitSDK/AUBase.h" "${_original}/AU/AudioUnitSDK/AUBase.cpp")
    set(FLOWS_PATCHED_JUCE_MODULES "${_generated}" PARENT_SCOPE)
endfunction()
