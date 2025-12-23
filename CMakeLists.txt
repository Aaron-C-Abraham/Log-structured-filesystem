cmake_minimum_required(VERSION 3.16)
project(lsfs C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Compiler flags
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Werror -D_FILE_OFFSET_BITS=64")
set(CMAKE_C_FLAGS_DEBUG "-g -O0 -DDEBUG")
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG")

# Find required packages
find_package(PkgConfig REQUIRED)
pkg_check_modules(FUSE3 REQUIRED fuse3)

# Include directories
include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${FUSE3_INCLUDE_DIRS}
)

# Link directories
link_directories(${FUSE3_LIBRARY_DIRS})

# Source files for the main library
set(LSFS_LIB_SOURCES
    src/io.c
    src/inode.c
    src/directory.c
    src/segment.c
    src/imap.c
    src/checkpoint.c
    src/gc.c
    src/fuse_ops.c
)

# Create static library
add_library(lsfs_lib STATIC ${LSFS_LIB_SOURCES})
target_link_libraries(lsfs_lib ${FUSE3_LIBRARIES} pthread)

# Main LSFS daemon
add_executable(lsfs src/main.c)
target_link_libraries(lsfs lsfs_lib ${FUSE3_LIBRARIES} pthread)

# mkfs.lsfs utility
add_executable(mkfs.lsfs tools/mkfs.lsfs.c)
target_link_libraries(mkfs.lsfs lsfs_lib)

# fsck.lsfs utility
add_executable(fsck.lsfs tools/fsck.lsfs.c)
target_link_libraries(fsck.lsfs lsfs_lib)

# lsfs-debug utility
add_executable(lsfs-debug tools/lsfs-debug.c)
target_link_libraries(lsfs-debug lsfs_lib)

# Install targets
install(TARGETS lsfs mkfs.lsfs fsck.lsfs lsfs-debug
    RUNTIME DESTINATION bin
)

# Enable testing
enable_testing()

# Add test directory
add_subdirectory(tests)
