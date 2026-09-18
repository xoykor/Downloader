cmake_minimum_required(VERSION 3.20)

project(
    downloader_c
    VERSION 0.1.0
    DESCRIPTION "Migração incremental do Downloader para C17"
    LANGUAGES C
)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(DOWNLOADER_WARNINGS_AS_ERRORS "Tratar avisos do compilador como erros" ON)

add_library(
    downloader_core
    STATIC
    src/domain.c
    src/application.c
)

target_include_directories(
    downloader_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(
        downloader_core
        PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wshadow
    )
    if(DOWNLOADER_WARNINGS_AS_ERRORS)
        target_compile_options(downloader_core PRIVATE -Werror)
    endif()
endif()

add_executable(downloader-c-cli src/cli/main.c)
target_link_libraries(downloader-c-cli PRIVATE downloader_core)

include(CTest)
if(BUILD_TESTING)
    add_executable(test_domain tests/test_domain.c)
    target_link_libraries(test_domain PRIVATE downloader_core)
    add_test(NAME domain COMMAND test_domain)

    add_executable(test_application tests/test_application.c)
    target_link_libraries(test_application PRIVATE downloader_core)
    add_test(NAME application COMMAND test_application)
endif()
