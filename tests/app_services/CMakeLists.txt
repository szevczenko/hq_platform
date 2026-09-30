cmake_minimum_required(VERSION 3.16)
project(hq_app_services_tests LANGUAGES C)

set(PLATFORM_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)
set(APP_SERVICES_DIR ${PLATFORM_ROOT}/src/hq_app_services)
set(TB_DIR ${PLATFORM_ROOT}/src/thingsboard)
set(OSAL_DIR ${PLATFORM_ROOT}/src/osal)
set(CJSON_DIR ${PLATFORM_ROOT}/third_party/cJSON)
set(UNITY_DIR ${PLATFORM_ROOT}/tests/unity/src)
find_package(Threads REQUIRED)

add_executable(hq_app_services_tests
    app_services_test.c
    osal_file_mock.c
    osal_test_support.c
    ${APP_SERVICES_DIR}/hq_json_doc.c
    ${APP_SERVICES_DIR}/hq_device_info.c
    ${TB_DIR}/tb_identity.c
    ${APP_SERVICES_DIR}/hq_app_state.c
    ${CJSON_DIR}/cJSON.c
    ${UNITY_DIR}/unity.c
)
set_target_properties(hq_app_services_tests PROPERTIES C_STANDARD 11)

target_include_directories(hq_app_services_tests PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/config
    ${APP_SERVICES_DIR}/include
    ${TB_DIR}/include
    ${OSAL_DIR}/include
    ${OSAL_DIR}/posix
    ${CJSON_DIR}
    ${UNITY_DIR}
)

target_compile_definitions(hq_app_services_tests PRIVATE CONFIG_HQ_PLATFORM_POSIX)
target_compile_options(hq_app_services_tests PRIVATE -Wall -Wextra -Werror -Wpedantic)
set_source_files_properties(${CJSON_DIR}/cJSON.c ${UNITY_DIR}/unity.c
    PROPERTIES COMPILE_OPTIONS "-w")

target_link_libraries(hq_app_services_tests PRIVATE Threads::Threads)
enable_testing()
add_test(NAME hq_app_services_tests COMMAND hq_app_services_tests)
set_tests_properties(hq_app_services_tests PROPERTIES LABELS "unit;platform;app_services")
