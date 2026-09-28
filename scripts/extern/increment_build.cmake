# Get the current git commit count and save it to GIT_COMMIT_COUNT
execute_process(
  COMMAND git rev-list --count HEAD
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  OUTPUT_VARIABLE GIT_COMMIT_COUNT
  OUTPUT_STRIP_TRAILING_WHITESPACE
)

# The build-number script runs on the build machine, so pick it by host, not
# by target: a cross build targets Windows from a Linux host. The shell copy
# is run through sh because its executable bit does not survive a Windows
# checkout.
if(CMAKE_HOST_WIN32)
  set(KISAK_BUILD_NUMBER_COMMAND ${SCRIPTS_DIR}/increment_build.cmd)
else()
  set(KISAK_BUILD_NUMBER_COMMAND sh ${SCRIPTS_DIR}/increment_build.sh)
endif()

add_custom_target(
  update_build_number
  COMMAND ${KISAK_BUILD_NUMBER_COMMAND} ${SRC_DIR} ${GIT_COMMIT_COUNT}
  COMMENT "Running build number script..."
)