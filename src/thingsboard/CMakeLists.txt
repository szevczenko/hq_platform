set(TB_PUBLIC_INCLUDES
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

set(TB_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/tb_client.c
  ${CMAKE_CURRENT_SOURCE_DIR}/tb_telemetry.c
  ${CMAKE_CURRENT_SOURCE_DIR}/tb_attributes.c
  ${CMAKE_CURRENT_SOURCE_DIR}/tb_rpc.c
  ${CMAKE_CURRENT_SOURCE_DIR}/tb_provision.c
  ${CMAKE_CURRENT_SOURCE_DIR}/tb_claim.c
)

if(ESP_PLATFORM)
  idf_component_register(SRCS ${TB_SOURCES}
                         INCLUDE_DIRS ${TB_PUBLIC_INCLUDES}
                         REQUIRES osal mongoose json)
else()
  add_library(hq_thingsboard STATIC ${TB_SOURCES})

  target_include_directories(hq_thingsboard
    PUBLIC ${TB_PUBLIC_INCLUDES}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../mongoose
            ${CMAKE_CURRENT_SOURCE_DIR}/../osal/include
            ${HQ_CJSON_INCLUDE_DIRS}
            ${HQ_CONFIG_DIR}
  )

  if(CONFIG_HQ_PLATFORM_POSIX)
    target_include_directories(hq_thingsboard
      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../osal/posix
    )
  elseif(CONFIG_HQ_PLATFORM_ESP)
    target_include_directories(hq_thingsboard
      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../osal/esp
    )
  endif()

  target_link_libraries(hq_thingsboard
    PUBLIC hq_osal hq_mongoose hq_cjson
  )
endif()
