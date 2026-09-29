set(HQ_INPUT_SERVICES_SOURCES
  "hq_button.c"
  "hq_factory_reset.c"
)
set(HQ_INPUT_SERVICES_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include")

if(ESP_PLATFORM)
  idf_component_register(
    SRCS ${HQ_INPUT_SERVICES_SOURCES}
    INCLUDE_DIRS ${HQ_INPUT_SERVICES_INCLUDE_DIRS}
    REQUIRES hq_hal
  )
else()
  add_library(hq_input STATIC ${HQ_INPUT_SERVICES_SOURCES})
  target_include_directories(hq_input PUBLIC ${HQ_INPUT_SERVICES_INCLUDE_DIRS})
  target_link_libraries(hq_input PUBLIC hq_hal)
endif()