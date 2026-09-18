cmake_minimum_required(VERSION 3.20)
project(downloader_c VERSION 1.0.0 DESCRIPTION "Downloader em C17" LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
add_compile_definitions(_POSIX_C_SOURCE=200809L)

option(DOWNLOADER_WARNINGS_AS_ERRORS "Tratar avisos como erros" ON)
option(DOWNLOADER_BUILD_DESKTOP "Compilar interface GTK4 quando disponível" ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(JSONC REQUIRED IMPORTED_TARGET json-c)
pkg_check_modules(SQLITE3 REQUIRED IMPORTED_TARGET sqlite3)
find_package(Threads REQUIRED)

set(DOWNLOADER_CORE_SOURCES
    src/domain.c
    src/application.c
    src/process.c
    src/media.c
    src/database.c
    src/publish.c
    src/hardware.c
    src/library.c
    src/engine.c
)

add_library(downloader_core STATIC ${DOWNLOADER_CORE_SOURCES})
target_include_directories(downloader_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(downloader_core PUBLIC PkgConfig::JSONC PkgConfig::SQLITE3 Threads::Threads m)
target_compile_definitions(downloader_core PRIVATE _POSIX_C_SOURCE=200809L)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(downloader_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wformat=2)
    if(DOWNLOADER_WARNINGS_AS_ERRORS)
        target_compile_options(downloader_core PRIVATE -Werror)
    endif()
endif()

add_executable(downloader-cli src/cli/main.c)
target_link_libraries(downloader-cli PRIVATE downloader_core)
target_compile_definitions(downloader-cli PRIVATE _POSIX_C_SOURCE=200809L)

if(DOWNLOADER_BUILD_DESKTOP)
    pkg_check_modules(GTK4 QUIET IMPORTED_TARGET gtk4)
    if(GTK4_FOUND)
        add_executable(downloader-desktop src/desktop/main.c)
        target_link_libraries(downloader-desktop PRIVATE downloader_core PkgConfig::GTK4)
        target_compile_definitions(downloader-desktop PRIVATE _POSIX_C_SOURCE=200809L)
    else()
        message(STATUS "GTK4 não encontrado: downloader-desktop não será compilado")
    endif()
endif()

include(CTest)
if(BUILD_TESTING)
    foreach(test_name domain application media process publish database library engine)
        add_executable(test_${test_name} tests/test_${test_name}.c)
        target_link_libraries(test_${test_name} PRIVATE downloader_core)
        if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
            # Mantém assert() ativo também nos testes Release/AppImage.
            target_compile_options(test_${test_name} PRIVATE -UNDEBUG)
        endif()
        add_test(NAME ${test_name} COMMAND test_${test_name})
    endforeach()
endif()
