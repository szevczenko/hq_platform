idf_component_register(
  SRCS main.c
  INCLUDE_DIRS .
  # `hq_hal` is the collision-free ESP-IDF component name for the portable HAL
  # (derived from src/hal/hq_hal).  We never declare `REQUIRES hal`, because
  # `hal` is ESP-IDF's own framework component.
  REQUIRES hq_hal osal
)
