add_executable(littlefs_util
  littlefs_util.c
)

target_include_directories(littlefs_util PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/../third_party/littlefs
  ${CMAKE_CURRENT_LIST_DIR}/../third_party/littlefs/bd
)

target_sources(littlefs_util PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/../third_party/littlefs/lfs.c
  ${CMAKE_CURRENT_LIST_DIR}/../third_party/littlefs/lfs_util.c
  ${CMAKE_CURRENT_LIST_DIR}/../third_party/littlefs/bd/lfs_filebd.c
)
