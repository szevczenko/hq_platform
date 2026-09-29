set(HQ_APP_SERVICES_SOURCES
  "hq_json_doc.c"
  "hq_device_info.c"
  "hq_app_state.c"
)
set(HQ_APP_SERVICES_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include")

if(ESP_PLATFORM)
  idf_component_register(
    SRCS ${HQ_APP_SERVICES_SOURCES}
    INCLUDE_DIRS ${HQ_APP_SERVICES_INCLUDE_DIRS}
    REQUIRES osal json
  )
else()
  add_library(hq_app_services STATIC ${HQ_APP_SERVICES_SOURCES})
  set_target_properties(hq_app_services PROPERTIES C_STANDARD 11)
  target_include_directories(hq_app_services PUBLIC ${HQ_APP_SERVICES_INCLUDE_DIRS})
  target_link_libraries(hq_app_services PUBLIC hq_osal hq_cjson)
endif()