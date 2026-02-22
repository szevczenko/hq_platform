#ifndef OSAL_ERROR_H
#define OSAL_ERROR_H

/**
 * @brief OSAL status codes
 */
typedef enum {
    OSAL_SUCCESS                     = 0,   /**< @brief Successful execution */
    OSAL_ERROR                       = -1,  /**< @brief Failed execution */
    OSAL_INVALID_POINTER             = -2,  /**< @brief Invalid pointer argument */
    OSAL_ERROR_ADDRESS_MISALIGNED    = -3,  /**< @brief Memory address is not properly aligned */
    OSAL_ERROR_TIMEOUT               = -4,  /**< @brief Operation timed out */
    OSAL_INVALID_INT_NUM             = -5,  /**< @brief Invalid interrupt number */
    OSAL_SEM_FAILURE                 = -6,  /**< @brief Semaphore operation failed */
    OSAL_SEM_TIMEOUT                 = -7,  /**< @brief Semaphore operation timed out */
    OSAL_QUEUE_EMPTY                 = -8,  /**< @brief Queue is empty */
    OSAL_QUEUE_FULL                  = -9,  /**< @brief Queue is full */
    OSAL_QUEUE_TIMEOUT               = -10, /**< @brief Queue operation timed out */
    OSAL_QUEUE_INVALID_SIZE          = -11, /**< @brief Invalid queue size */
    OSAL_QUEUE_ID_ERROR              = -12, /**< @brief Invalid queue ID */
    OSAL_ERR_NAME_TOO_LONG           = -13, /**< @brief Name exceeds maximum length */
    OSAL_ERR_NO_FREE_IDS             = -14, /**< @brief No free resource IDs available */
    OSAL_ERR_NAME_TAKEN              = -15, /**< @brief Resource name already in use */
    OSAL_ERR_INVALID_ID              = -16, /**< @brief Invalid resource ID */
    OSAL_ERR_NAME_NOT_FOUND          = -17, /**< @brief Resource name not found */
    OSAL_ERR_SEM_NOT_FULL            = -18, /**< @brief Semaphore is not full */
    OSAL_ERR_INVALID_PRIORITY        = -19, /**< @brief Invalid task/thread priority */
    OSAL_INVALID_SEM_VALUE           = -20, /**< @brief Invalid semaphore value */
    /* -21 to -26: reserved for future use */
    OSAL_ERR_FILE                    = -27, /**< @brief File operation error */
    OSAL_ERR_NOT_IMPLEMENTED         = -28, /**< @brief Feature not implemented */
    OSAL_TIMER_ERR_INVALID_ARGS      = -29, /**< @brief Invalid timer arguments */
    OSAL_TIMER_ERR_TIMER_ID          = -30, /**< @brief Invalid timer ID */
    OSAL_TIMER_ERR_UNAVAILABLE       = -31, /**< @brief Timer resource unavailable */
    OSAL_TIMER_ERR_INTERNAL          = -32, /**< @brief Internal timer error */
    OSAL_ERR_OBJECT_IN_USE           = -33, /**< @brief Resource is currently in use */
    OSAL_ERR_BAD_ADDRESS             = -34, /**< @brief Invalid memory address */
    OSAL_ERR_INCORRECT_OBJ_STATE     = -35, /**< @brief Object in incorrect state for operation */
    OSAL_ERR_INCORRECT_OBJ_TYPE      = -36, /**< @brief Incorrect object type */
    OSAL_ERR_STREAM_DISCONNECTED     = -37, /**< @brief Stream connection lost */
    OSAL_ERR_OPERATION_NOT_SUPPORTED = -38, /**< @brief Operation not supported on this object */
    /* -39: reserved for future use */
    OSAL_ERR_INVALID_SIZE            = -40, /**< @brief Invalid size parameter */
    OSAL_ERR_OUTPUT_TOO_LARGE        = -41, /**< @brief Output size exceeds limit */
    OSAL_ERR_INVALID_ARGUMENT        = -42, /**< @brief Invalid argument value */
    OSAL_ERR_TRY_AGAIN               = -43, /**< @brief Temporary failure, retry operation */
    OSAL_ERR_EMPTY_SET               = -44  /**< @brief Lookup returned no results */
} osal_status_t;

/**
 * @brief Convert an status to a human-readable string
 *
 * @param[in]  status  Status
 *
 * @return Status string or "unknown error" if
 */
const char *osal_get_status_name(osal_status_t status);

#endif /* OSAL_ERROR_H */
