add_subdirectory(osal)
add_subdirectory(hal)
add_subdirectory(mongoose)
add_subdirectory(wifi)
add_subdirectory(thingsboard)
add_subdirectory(cmd)

# Wi-Fi provisioning is an optional component: it is only compiled when
# CONFIG_WIFI_HTTP_PROVISIONING is enabled.
if(CONFIG_WIFI_HTTP_PROVISIONING)
  add_subdirectory(wifi_provisioning)
endif()
