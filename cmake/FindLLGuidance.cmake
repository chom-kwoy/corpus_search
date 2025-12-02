CPMAddPackage(
  NAME llguidance
  GITHUB_REPOSITORY "guidance-ai/llguidance"
  GIT_TAG v1.4.0
)

add_library(LLGuidance::LLGuidance STATIC IMPORTED)

set_target_properties(LLGuidance::LLGuidance PROPERTIES
    IMPORTED_LOCATION "${llguidance_SOURCE_DIR}/target/release/libllguidance.a"
    INTERFACE_INCLUDE_DIRECTORIES "${llguidance_SOURCE_DIR}/parser"
)

find_program(CARGO_EXECUTABLE NAMES cargo)
add_custom_target(cargo_build
    COMMAND ${CARGO_EXECUTABLE} build --release
    WORKING_DIRECTORY "${llguidance_SOURCE_DIR}/parser"
)

add_dependencies(LLGuidance::LLGuidance cargo_build)
