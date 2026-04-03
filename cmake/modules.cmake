set(HQ_CJSON_DIR ${CMAKE_CURRENT_LIST_DIR}/../third_party/cJSON)
set(HQ_CJSON_SOURCES ${HQ_CJSON_DIR}/cJSON.c)
set(HQ_CJSON_INCLUDE_DIRS ${HQ_CJSON_DIR})

if(NOT ESP_PLATFORM)
  add_library(hq_cjson STATIC ${HQ_CJSON_SOURCES})
  target_include_directories(hq_cjson PUBLIC ${HQ_CJSON_INCLUDE_DIRS})
endif()
