/**
 * @file hq_config.h
 * @brief Minimal hq_config.h for app_services host tests
 *
 * The platform OSAL headers include hq_config.h, which the platform build
 * system generates from defconfigs (HQ_CONFIG_DIR; products typically use
 * CONFIG_OSAL_LOG_LEVEL=0, i.e. DEBUG).  The host test build does not run
 * the platform configure step, so this stub supplies the one setting the
 * compiled OSAL logging macros depend on: the log level.  OSAL log levels:
 * 0=DEBUG, 1=INFO, 2=WARNING, 3=ERROR, 4=NONE.
 */
#ifndef HQ_CONFIG_H
#define HQ_CONFIG_H

#ifndef CONFIG_OSAL_LOG_LEVEL
#define CONFIG_OSAL_LOG_LEVEL 0 /* DEBUG */
#endif

#endif /* HQ_CONFIG_H */
