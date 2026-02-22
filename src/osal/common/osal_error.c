#include "osal_error.h"

const char *osal_get_status_name(osal_status_t status)
{
    switch (status)
    {
        case OSAL_SUCCESS:
            return "OSAL_SUCCESS";
        case OSAL_ERROR:
            return "OSAL_ERROR";
        case OSAL_INVALID_POINTER:
            return "OSAL_INVALID_POINTER";
        case OSAL_ERROR_ADDRESS_MISALIGNED:
            return "OSAL_ERROR_ADDRESS_MISALIGNED";
        case OSAL_ERROR_TIMEOUT:
            return "OSAL_ERROR_TIMEOUT";
        case OSAL_INVALID_INT_NUM:
            return "OSAL_INVALID_INT_NUM";
        case OSAL_SEM_FAILURE:
            return "OSAL_SEM_FAILURE";
        case OSAL_SEM_TIMEOUT:
            return "OSAL_SEM_TIMEOUT";
        case OSAL_QUEUE_EMPTY:
            return "OSAL_QUEUE_EMPTY";
        case OSAL_QUEUE_FULL:
            return "OSAL_QUEUE_FULL";
        case OSAL_QUEUE_TIMEOUT:
            return "OSAL_QUEUE_TIMEOUT";
        case OSAL_QUEUE_INVALID_SIZE:
            return "OSAL_QUEUE_INVALID_SIZE";
        case OSAL_QUEUE_ID_ERROR:
            return "OSAL_QUEUE_ID_ERROR";
        case OSAL_ERR_NAME_TOO_LONG:
            return "OSAL_ERR_NAME_TOO_LONG";
        case OSAL_ERR_NO_FREE_IDS:
            return "OSAL_ERR_NO_FREE_IDS";
        case OSAL_ERR_NAME_TAKEN:
            return "OSAL_ERR_NAME_TAKEN";
        case OSAL_ERR_INVALID_ID:
            return "OSAL_ERR_INVALID_ID";
        case OSAL_ERR_NAME_NOT_FOUND:
            return "OSAL_ERR_NAME_NOT_FOUND";
        case OSAL_ERR_SEM_NOT_FULL:
            return "OSAL_ERR_SEM_NOT_FULL";
        case OSAL_ERR_INVALID_PRIORITY:
            return "OSAL_ERR_INVALID_PRIORITY";
        case OSAL_INVALID_SEM_VALUE:
            return "OSAL_INVALID_SEM_VALUE";
        case OSAL_ERR_FILE:
            return "OSAL_ERR_FILE";
        case OSAL_ERR_NOT_IMPLEMENTED:
            return "OSAL_ERR_NOT_IMPLEMENTED";
        case OSAL_TIMER_ERR_INVALID_ARGS:
            return "OSAL_TIMER_ERR_INVALID_ARGS";
        case OSAL_TIMER_ERR_TIMER_ID:
            return "OSAL_TIMER_ERR_TIMER_ID";
        case OSAL_TIMER_ERR_UNAVAILABLE:
            return "OSAL_TIMER_ERR_UNAVAILABLE";
        case OSAL_TIMER_ERR_INTERNAL:
            return "OSAL_TIMER_ERR_INTERNAL";
        case OSAL_ERR_OBJECT_IN_USE:
            return "OSAL_ERR_OBJECT_IN_USE";
        case OSAL_ERR_BAD_ADDRESS:
            return "OSAL_ERR_BAD_ADDRESS";
        case OSAL_ERR_INCORRECT_OBJ_STATE:
            return "OSAL_ERR_INCORRECT_OBJ_STATE";
        case OSAL_ERR_INCORRECT_OBJ_TYPE:
            return "OSAL_ERR_INCORRECT_OBJ_TYPE";
        case OSAL_ERR_STREAM_DISCONNECTED:
            return "OSAL_ERR_STREAM_DISCONNECTED";
        case OSAL_ERR_OPERATION_NOT_SUPPORTED:
            return "OSAL_ERR_OPERATION_NOT_SUPPORTED";
        case OSAL_ERR_INVALID_SIZE:
            return "OSAL_ERR_INVALID_SIZE";
        case OSAL_ERR_OUTPUT_TOO_LARGE:
            return "OSAL_ERR_OUTPUT_TOO_LARGE";
        case OSAL_ERR_INVALID_ARGUMENT:
            return "OSAL_ERR_INVALID_ARGUMENT";
        case OSAL_ERR_TRY_AGAIN:
            return "OSAL_ERR_TRY_AGAIN";
        case OSAL_ERR_EMPTY_SET:
            return "OSAL_ERR_EMPTY_SET";
        default:
            break;
    }

    if ((status >= -26 && status <= -21) || status == -39)
    {
        return "OSAL_ERR_RESERVED";
    }

    return "unknown error";
}
