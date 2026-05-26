set(CMD_PUBLIC_INCLUDES
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${HQ_CONFIG_DIR}
)

get_filename_component(HQ_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

set(CMD_EMBEDDED_CLI_DIR ${CMAKE_CURRENT_LIST_DIR}/../../third_party/embedded-cli)
set(CMD_LITTLEFS_DIR ${HQ_REPO_ROOT}/third_party/littlefs)
set(CMD_OSAL_POSIX_DIR ${HQ_REPO_ROOT}/src/osal/posix)
set(CMD_OSAL_ESP_DIR ${HQ_REPO_ROOT}/src/osal/esp)

set(CMD_PRIVATE_INCLUDES
  ${CMD_EMBEDDED_CLI_DIR}/lib/include
)

file(GLOB CMD_COMMON_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/*.c
)

file(GLOB CMD_COMMAND_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/commands/*.c
)

if(CONFIG_HQ_PLATFORM_POSIX)
  file(GLOB CMD_PLATFORM_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/platforms/posix/*.c
  )
  set(CMD_PLATFORM_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/platforms/posix
    ${CMD_OSAL_POSIX_DIR}
    ${CMD_LITTLEFS_DIR}
    ${CMD_LITTLEFS_DIR}/bd
  )
elseif(CONFIG_HQ_PLATFORM_ESP OR ESP_PLATFORM)
  file(GLOB CMD_PLATFORM_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/platforms/esp/*.c
  )
  set(CMD_PLATFORM_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/platforms/esp
    ${CMD_OSAL_ESP_DIR}
    ${CMD_LITTLEFS_DIR}
  )
endif()

set(CMD_ALL_SOURCES
  ${CMD_COMMON_SOURCES}
  ${CMD_COMMAND_SOURCES}
  ${CMD_PLATFORM_SOURCES}
  ${CMD_EMBEDDED_CLI_DIR}/lib/src/embedded_cli.c
)

if(ESP_PLATFORM)
  set(CMD_REQUIRES osal wifi mongoose)
  if(CONFIG_CMD_ESP_OUTPUT_UART)
    list(APPEND CMD_REQUIRES driver)
  endif()

  idf_component_register(SRCS ${CMD_ALL_SOURCES}
                         INCLUDE_DIRS ${CMD_PUBLIC_INCLUDES}
                         PRIV_INCLUDE_DIRS ${CMD_PRIVATE_INCLUDES} ${CMD_PLATFORM_INCLUDES}
                         REQUIRES ${CMD_REQUIRES})
else()
  add_library(hq_cmd STATIC ${CMD_ALL_SOURCES})
  target_include_directories(hq_cmd
    PUBLIC ${CMD_PUBLIC_INCLUDES}
    PRIVATE ${CMD_PRIVATE_INCLUDES} ${CMD_PLATFORM_INCLUDES}
  )
  target_link_libraries(hq_cmd PUBLIC hq_osal hq_wifi hq_mongoose hq_thingsboard)
endif()
