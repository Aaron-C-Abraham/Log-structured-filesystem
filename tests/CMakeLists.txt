# LSFS Tests CMakeLists.txt

# Add test script as a custom target
add_custom_target(test-lsfs
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/test.sh
    DEPENDS lsfs mkfs.lsfs fsck.lsfs
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running LSFS tests"
)
