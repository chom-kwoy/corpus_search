CPMAddPackage(
  NAME llguidance
  GITHUB_REPOSITORY "guidance-ai/llguidance"
  GIT_TAG v1.4.0
)

execute_process(
    COMMAND cargo build --release
    WORKING_DIRECTORY "${llguidance_SOURCE_DIR}/parser"
    RESULT_VARIABLE rs_build_result
)

if(NOT rs_build_result EQUAL 0)
    message(FATAL_ERROR "llguidance build failed with code ${rs_build_result}.")
endif()

# 1. Find the library
find_library(LLGUIDANCE_LIBRARY
    NAMES "libllguidance.a"
    # Look in a user-provided root, or standard system paths
    HINTS "${llguidance_SOURCE_DIR}/target/release"
    NO_DEFAULT_PATH
)

# 2. Find the include directory
# IMPORTANT: Replace 'parser.h' with an actual unique header filename inside that folder
find_path(LLGUIDANCE_INCLUDE_DIR
    NAMES llguidance.h
    HINTS "${llguidance_SOURCE_DIR}/parser"
)

# 3. Standard handling
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(LLGuidance
    REQUIRED_VARS LLGUIDANCE_LIBRARY LLGUIDANCE_INCLUDE_DIR
)

# 4. Create the Imported Target (Modern CMake style)
if(LLGuidance_FOUND AND NOT TARGET LLGuidance::LLGuidance)
    add_library(LLGuidance::LLGuidance STATIC IMPORTED)

    set_target_properties(LLGuidance::LLGuidance PROPERTIES
        IMPORTED_LOCATION "${LLGUIDANCE_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${LLGUIDANCE_INCLUDE_DIR}"
    )
endif()

# Hide raw variables from the GUI to keep it clean
mark_as_advanced(LLGUIDANCE_LIBRARY LLGUIDANCE_INCLUDE_DIR)
