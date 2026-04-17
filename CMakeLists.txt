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
    kernel/json.c
    kernel/loader.c
    kernel/permissions.c
    kernel/process.c
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
    # Built-in demo bundle smoke tests
    add_test(NAME uracil_describe COMMAND uracil describe)
    add_test(NAME uracil_permissions COMMAND uracil permissions)
    add_test(NAME uracil_version COMMAND uracil --version)
    add_test(NAME uracil_help COMMAND uracil --help)

    # JSON parser tests
    add_executable(test_json tests/test_json.c)
    target_link_libraries(test_json PRIVATE uracil_core)
    target_compile_options(test_json PRIVATE ${URACIL_WARNING_FLAGS})
    add_test(NAME test_json COMMAND test_json)

    # Bundle loader tests
    add_executable(test_loader tests/test_loader.c)
    target_link_libraries(test_loader PRIVATE uracil_core)
    target_compile_options(test_loader PRIVATE ${URACIL_WARNING_FLAGS})
    add_test(NAME test_loader COMMAND test_loader)
    set_tests_properties(test_loader PROPERTIES
        ENVIRONMENT "URACIL_TEST_MANIFEST_DIR=${CMAKE_SOURCE_DIR}/examples")

    # Permission tests
    add_executable(test_permissions tests/test_permissions.c)
    target_link_libraries(test_permissions PRIVATE uracil_core)
    target_compile_options(test_permissions PRIVATE ${URACIL_WARNING_FLAGS})
    add_test(NAME test_permissions COMMAND test_permissions)
endif()
