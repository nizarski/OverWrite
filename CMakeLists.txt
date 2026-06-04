cmake_minimum_required(VERSION 3.14)
project(OverWrite C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W4 /utf-8)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -O2)
endif()

set(OVERWRITE_SOURCES
    src/main.c
    src/common.c
    src/rng.c
    src/chacha20.c
    src/partition.c
    src/target.c
    src/wipe.c
    src/progress.c
    src/safety.c
    src/free_space.c
    src/hidden_area.c
    src/slack.c
    src/devlist.c
    src/android.c
    src/format.c
)

if(WIN32)
    list(APPEND OVERWRITE_SOURCES src/platform_win.c)
else()
    list(APPEND OVERWRITE_SOURCES src/platform_linux.c)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        list(APPEND OVERWRITE_SOURCES src/secure_erase_linux.c)
    endif()
endif()

add_executable(overwrite ${OVERWRITE_SOURCES})

target_include_directories(overwrite PRIVATE include)

if(WIN32)
    target_link_libraries(overwrite PRIVATE bcrypt)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(overwrite PRIVATE Threads::Threads)
endif()

install(TARGETS overwrite RUNTIME DESTINATION bin)

install(PROGRAMS
    gui/overwrite_gui.py
    scripts/overwrite-gui.sh
    DESTINATION bin
)
if(WIN32)
    install(PROGRAMS scripts/overwrite-gui.ps1 DESTINATION bin)
endif()
