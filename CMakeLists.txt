cmake_minimum_required(VERSION 3.20)

project(uracil VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(URACIL_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)

include(CTest)

set(URACIL_WARNING_FLAGS
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wshadow
)

if(URACIL_WARNINGS_AS_ERRORS)
    list(APPEND URACIL_WARNING_FLAGS -Werror)
endif()

add_library(uracil_core
    arch/host/platform.c
    app/demo_bundle.c
    kernel/bundle.c
    kernel/permissions.c
    kernel/runtime.c
)

target_include_directories(uracil_core
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_SOURCE_DIR}/app
)

target_compile_options(uracil_core PRIVATE ${URACIL_WARNING_FLAGS})

add_executable(uracil
    app/main.c
)

target_link_libraries(uracil PRIVATE uracil_core)
target_include_directories(uracil PRIVATE ${CMAKE_SOURCE_DIR}/app)
target_compile_options(uracil PRIVATE ${URACIL_WARNING_FLAGS})

if(BUILD_TESTING)
    add_test(NAME uracil_describe COMMAND uracil describe)
    add_test(NAME uracil_permissions COMMAND uracil permissions)
endif()
