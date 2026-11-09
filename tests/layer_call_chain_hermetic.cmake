# Runs one call-chain scenario against a private copy of the layer and a chosen
# ini, in its own directory.
#
# Why: the scenario reads the ini beside the DLL it was given to know which
# pipeline the layer will run, and the layer itself reads that same file. When
# these scenarios ran against the build directory, any leftover repro_vr.ini from
# an earlier run silently changed the pipeline under test - the layer ran shallow
# while the expectations described deep, and every swapchain and frame count came
# out one short. Measured: removing that file made the same three scenarios pass.
#
# Arguments: LAYER_DLL, CALL_CHAIN, PIPELINE_INI, WORK_DIR, MODE.
if(NOT DEFINED LAYER_DLL OR NOT DEFINED CALL_CHAIN OR NOT DEFINED PIPELINE_INI
   OR NOT DEFINED WORK_DIR OR NOT DEFINED MODE)
    message(FATAL_ERROR "layer_call_chain_hermetic.cmake needs LAYER_DLL, CALL_CHAIN, PIPELINE_INI, WORK_DIR and MODE")
endif()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

get_filename_component(layer_name "${LAYER_DLL}" NAME)
file(COPY "${LAYER_DLL}" DESTINATION "${WORK_DIR}")

# The layer resolves repro_vr.ini before the legacy name, so the scenario's ini
# is placed under that name: nothing else in the directory can then win.
file(READ "${PIPELINE_INI}" ini_contents)
file(WRITE "${WORK_DIR}/repro_vr.ini" "${ini_contents}")

execute_process(
    COMMAND "${CALL_CHAIN}" "${WORK_DIR}/${layer_name}"
            "${WORK_DIR}/scenario.log" "${MODE}"
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "call-chain scenario ${MODE} failed (exit ${result}); log: ${WORK_DIR}/scenario.log")
endif()
