cmake_minimum_required(VERSION 3.10)

project(unix_group_fs_course_design C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_executable(ugfs
    src/main.c
    src/runtime.c
    src/disk.c
    src/inode.c
    src/alloc.c
    src/directory.c
    src/users.c
    src/format.c
    src/commands.c
    src/file_ops.c
    src/shell.c
)

target_include_directories(ugfs PRIVATE include)

if (MSVC)
    target_compile_options(ugfs PRIVATE /W4)
else()
    target_compile_options(ugfs PRIVATE -Wall -Wextra -pedantic)
endif()
