set(NET_PUBLIC_INCLUDES
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${HQ_CONFIG_DIR}
)

if(ESP_PLATFORM)
  idf_component_register(
    SRCS "hq_net.c"
    INCLUDE_DIRS ${NET_PUBLIC_INCLUDES}
    PRIV_REQUIRES osal wifi
  )
else()
  add_library(hq_net STATIC hq_net.c)
  target_include_directories(hq_net PUBLIC ${NET_PUBLIC_INCLUDES})
  target_link_libraries(hq_net PUBLIC hq_osal PRIVATE hq_wifi)
endif()