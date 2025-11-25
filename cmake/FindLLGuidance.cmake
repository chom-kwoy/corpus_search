# 1. Find the library
find_library(LLGUIDANCE_LIBRARY
    NAMES llguidance
    # Look in a user-provided root, or standard system paths
    HINTS "${LLGUIDANCE_ROOT}/target/release" ENV LLGUIDANCE_ROOT
    PATH_SUFFIXES lib
)

# 2. Find the include directory
# IMPORTANT: Replace 'parser.h' with an actual unique header filename inside that folder
find_path(LLGUIDANCE_INCLUDE_DIR
    NAMES parser.h
    HINTS "${LLGUIDANCE_ROOT}/parser" ENV LLGUIDANCE_ROOT
    PATH_SUFFIXES include parser
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
