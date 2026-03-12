# Asset component — auto-discovers WebP files and generates C byte arrays at build time.
#
# To add a new brand boot animation: drop {brand}_boot.webp into resources/webp/
# To add a 2x variant: drop {brand}_boot_2x.webp alongside it
# To add a new common asset: drop {name}.webp (and optional {name}_2x.webp)

# Register component first — required before early expansion boundary.
idf_component_register(SRCS "assets.cpp" INCLUDE_DIRS ".")

# Everything below only runs during the real build, not early expansion.
if(CMAKE_BUILD_EARLY_EXPANSION)
    return()
endif()

set(PROJECT_ROOT "${CMAKE_SOURCE_DIR}")
set(RESOURCES_WEBP_DIR "${PROJECT_ROOT}/resources/webp")
set(TOOLS_DIR "${PROJECT_ROOT}/tools")
set(CONVERT_TOOL "${TOOLS_DIR}/webp_to_c.h.py")
set(GENERATE_TOOL "${TOOLS_DIR}/generate_assets.py")
set(GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")

# ── Step 1: Run the generator to discover assets and create headers ──
execute_process(
    COMMAND ${PYTHON} "${GENERATE_TOOL}" "${RESOURCES_WEBP_DIR}" "${GEN_DIR}"
    WORKING_DIRECTORY "${PROJECT_ROOT}"
    RESULT_VARIABLE GEN_RESULT
)
if(NOT GEN_RESULT EQUAL 0)
    message(FATAL_ERROR "generate_assets.py failed (exit ${GEN_RESULT})")
endif()

# ── Step 2: Read the jobs list and create conversion commands ────────
file(STRINGS "${GEN_DIR}/asset_jobs.txt" ASSET_JOBS)

set(GENERATED_ASSETS "")
foreach(JOB ${ASSET_JOBS})
    # Parse "webp_filename:c_filename:symbol"
    string(REPLACE ":" ";" JOB_PARTS "${JOB}")
    list(GET JOB_PARTS 0 WEBP_FILE)
    list(GET JOB_PARTS 1 C_FILE)
    list(GET JOB_PARTS 2 SYMBOL)

    set(INPUT "${RESOURCES_WEBP_DIR}/${WEBP_FILE}")
    set(OUTPUT "${GEN_DIR}/${C_FILE}")

    add_custom_command(
        OUTPUT "${OUTPUT}"
        COMMAND ${PYTHON} "${CONVERT_TOOL}" "${INPUT}" "${OUTPUT}" "${SYMBOL}"
        DEPENDS "${INPUT}" "${CONVERT_TOOL}"
        COMMENT "Generating ${C_FILE} from ${WEBP_FILE}"
        VERBATIM
    )
    list(APPEND GENERATED_ASSETS "${OUTPUT}")
endforeach()

# ── Step 3: Wire up build dependencies ───────────────────────────────
# Generated files (asset_data.h, asset_registry.inc, _c files) are in GEN_DIR
target_include_directories(${COMPONENT_LIB} PRIVATE "${GEN_DIR}")

# Ensure all _c files are generated before compilation
add_custom_target(generate_assets DEPENDS ${GENERATED_ASSETS})
add_dependencies(${COMPONENT_LIB} generate_assets)
