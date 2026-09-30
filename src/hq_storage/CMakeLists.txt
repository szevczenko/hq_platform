set(HQ_STORAGE_SOURCES "hq_storage.c")
set(HQ_STORAGE_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include")

if(ESP_PLATFORM)
  idf_component_register(
    SRCS ${HQ_STORAGE_SOURCES}
    INCLUDE_DIRS ${HQ_STORAGE_INCLUDE_DIRS}
    REQUIRES osal
  )
else()
  add_library(hq_storage STATIC ${HQ_STORAGE_SOURCES})
  target_include_directories(hq_storage PUBLIC ${HQ_STORAGE_INCLUDE_DIRS})
  target_link_libraries(hq_storage PUBLIC hq_osal)
endif()