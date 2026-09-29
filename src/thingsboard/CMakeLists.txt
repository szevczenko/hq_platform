set(TB_PUBLIC_INCLUDES
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

set(TB_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/tb_client.c
  ${CMAKE_CURRENT_SOURCE_DIR}/tb_telemetry.c
  ${CMAKE_CURRENT_SOURCE_DIR}/tb_attributes.c
  ${CMAKE_CURRENT_SOURCE_DIR}/tb_rpc.c
  ${CMAKE_CURRENT_SOURCE_DIR}/tb_state_sync.c
  ${CMAKE_CURRENT_SOURCE_DIR}/tb_provision.c
  ${CMAKE_CURRENT_SOURCE_DIR}/tb_enrollment.c
  ${CMAKE_CURRENT_SOURCE_DIR}/tb_claim.c
  ${CMAKE_CURRENT_SOURCE_DIR}/tb_firmware_update.c
  ${CMAKE_CURRENT_SOURCE_DIR}/tb_ota_manager.c
  ${CMAKE_CURRENT_SOURCE_DIR}/tb_identity.c
)

if(ESP_PLATFORM)
  # The ThingsBoard public headers expose only opaque handles and standard
  # types; osal, mongoose and json (cJSON) are used solely by the
  # implementation, so they are declared as private requirements.
  # tb_identity.h exposes hq_json_doc status codes (hq_app_services).
  idf_component_register(SRCS ${TB_SOURCES}
                         INCLUDE_DIRS ${TB_PUBLIC_INCLUDES}
                         REQUIRES hq_app_services
                         PRIV_REQUIRES osal mongoose json)
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
    PUBLIC hq_osal hq_mongoose hq_cjson hq_app_services
  )
endif()
