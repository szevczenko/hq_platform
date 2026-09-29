add_subdirectory(osal)
add_subdirectory(hq_storage)
add_subdirectory(hq_app_services)
add_subdirectory(hq_input)
add_subdirectory(hal)
add_subdirectory(mongoose)
add_subdirectory(wifi)
add_subdirectory(hq_net)
add_subdirectory(thingsboard)
add_subdirectory(cmd)

# Wi-Fi provisioning is an optional component: it is only compiled when
# CONFIG_WIFI_HTTP_PROVISIONING is enabled.
if(CONFIG_WIFI_HTTP_PROVISIONING)
  add_subdirectory(wifi_provisioning)
endif()
