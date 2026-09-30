#include "unity.h"

#include "hq_device_info.h"
#include "hq_app_state.h"
#include "hq_json_doc.h"
#include "osal_file_mock.h"
#include "osal_test_support.h"
#include "tb_identity.h"

static uint32_t s_state_now_ms;
static int s_state_callback_order[32];
static int s_state_callback_count;
static bool s_state_initialized;

static uint32_t state_test_now(void)
{
    return s_state_now_ms;
}

static void state_test_safe(hq_app_state_t state,
                            hq_app_state_event_t event,
                            hq_app_transition_owner_t owner,
                            uint32_t session)
{
    (void)state;
    (void)event;
    (void)owner;
    (void)session;
    s_state_callback_order[s_state_callback_count++] = 1;
}

static void state_test_enter(hq_app_state_t from,
                             hq_app_state_t to,
                             hq_app_state_event_t event,
                             hq_app_transition_owner_t owner,
                             uint32_t session)
{
    (void)from;
    (void)to;
    (void)event;
    (void)owner;
    (void)session;
    s_state_callback_order[s_state_callback_count++] = 2;
}

static void state_test_observer(hq_app_state_t from,
                                hq_app_state_t to,
                                hq_app_state_event_t event,
                                hq_app_transition_owner_t owner,
                                uint32_t session)
{
    (void)from;
    (void)to;
    (void)event;
    (void)owner;
    (void)session;
    s_state_callback_order[s_state_callback_count++] = 3;
}

static hq_app_state_config_t state_test_config(void)
{
    hq_app_state_config_t config = {0};

    config.now_ms = state_test_now;
    config.retry_initial_delay_ms = 1000u;
    config.retry_max_delay_ms = 4000u;
    config.retry_max_attempts = 2u;
    config.watchdog_timeout_ms = 60000u;
    config.on_state_enter = state_test_enter;
    config.on_safe_state = state_test_safe;
    config.observer = state_test_observer;
    return config;
}

void setUp(void)
{
    osal_file_mock_reset();
    tb_identity_clear();
    osal_test_log_reset();
    s_state_now_ms = 0u;
    s_state_callback_count = 0;
    s_state_initialized = false;
}

void tearDown(void)
{
    if (s_state_initialized)
    {
        hq_app_state_deinit();
        s_state_initialized = false;
    }
    tb_identity_clear();
}

static void test_app_state_callbacks_are_synchronous_and_ordered(void)
{
    hq_app_state_config_t config = state_test_config();

    TEST_ASSERT_EQUAL_INT(HQ_APP_STATE_OK, hq_app_state_init(&config));
    s_state_initialized = true;
    TEST_ASSERT_EQUAL_INT(HQ_APP_STATE_OK, hq_app_state_start());
    TEST_ASSERT_EQUAL_INT(HQ_APP_STATE_FILESYSTEM, hq_app_state_current());
    TEST_ASSERT_EQUAL_INT(3, s_state_callback_count);
    TEST_ASSERT_EQUAL_INT(1, s_state_callback_order[0]);
    TEST_ASSERT_EQUAL_INT(2, s_state_callback_order[1]);
    TEST_ASSERT_EQUAL_INT(3, s_state_callback_order[2]);

    s_state_callback_count = 0;
    TEST_ASSERT_EQUAL_INT(HQ_APP_STATE_OK,
        hq_app_state_deliver(HQ_APP_EVENT_FS_OK, HQ_APP_OWNER_FILESYSTEM));
    TEST_ASSERT_EQUAL_INT(HQ_APP_STATE_CONFIGURATION, hq_app_state_current());
    TEST_ASSERT_EQUAL_INT(3, s_state_callback_count);
    TEST_ASSERT_EQUAL_INT(1, s_state_callback_order[0]);
    TEST_ASSERT_EQUAL_INT(2, s_state_callback_order[1]);
    TEST_ASSERT_EQUAL_INT(3, s_state_callback_order[2]);

    s_state_callback_count = 0;
    TEST_ASSERT_EQUAL_INT(HQ_APP_STATE_ERR_STALE,
        hq_app_state_deliver_session(HQ_APP_EVENT_DISCONNECTED,
                                     HQ_APP_OWNER_NETWORK,
                                     hq_app_state_session() - 1u));
    TEST_ASSERT_EQUAL_INT(1, s_state_callback_count);
    TEST_ASSERT_EQUAL_INT(1, s_state_callback_order[0]);
}

static void test_app_state_retry_returns_to_failed_stage_after_backoff(void)
{
    hq_app_state_config_t config = state_test_config();

    TEST_ASSERT_EQUAL_INT(HQ_APP_STATE_OK, hq_app_state_init(&config));
    s_state_initialized = true;
    TEST_ASSERT_EQUAL_INT(HQ_APP_STATE_OK, hq_app_state_start());
    TEST_ASSERT_EQUAL_INT(HQ_APP_STATE_OK,
        hq_app_state_deliver(HQ_APP_EVENT_FS_OK, HQ_APP_OWNER_FILESYSTEM));
    TEST_ASSERT_EQUAL_INT(HQ_APP_STATE_OK,
        hq_app_state_deliver(HQ_APP_EVENT_CONFIG_OK,
                             HQ_APP_OWNER_CONFIGURATION));
    TEST_ASSERT_EQUAL_INT(HQ_APP_STATE_OK,
        hq_app_state_deliver(HQ_APP_EVENT_NETWORK_FAILED,
                             HQ_APP_OWNER_NETWORK));
    TEST_ASSERT_EQUAL_INT(HQ_APP_STATE_SAFE_OFF, hq_app_state_current());
    TEST_ASSERT_TRUE(hq_app_state_retry_pending());
    TEST_ASSERT_EQUAL_UINT32(1000u, hq_app_state_retry_delay_ms());

    TEST_ASSERT_EQUAL_INT(HQ_APP_STATE_OK, hq_app_state_poll_at(999u));
    TEST_ASSERT_EQUAL_INT(HQ_APP_STATE_SAFE_OFF, hq_app_state_current());
    TEST_ASSERT_EQUAL_INT(HQ_APP_STATE_OK, hq_app_state_poll_at(1000u));
    TEST_ASSERT_EQUAL_INT(HQ_APP_STATE_NETWORK, hq_app_state_current());
    TEST_ASSERT_EQUAL_UINT32(1u, hq_app_state_retry_attempts_used());
}

static void test_strict_json_rejects_duplicate_and_nul_escape(void)
{
    const char duplicate[] = "{\"x\":1,\"x\":2}";
    const char embedded_nul[] = "{\"x\":\"\\u0000\"}";
    TEST_ASSERT_EQUAL_INT(HQ_JSON_DOC_ERR_MALFORMED,
        hq_json_doc_validate(duplicate, sizeof(duplicate) - 1U, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(HQ_JSON_DOC_ERR_MALFORMED,
        hq_json_doc_validate(embedded_nul, sizeof(embedded_nul) - 1U,
                             NULL, NULL));
}

static void test_device_info_uses_caller_path_and_strict_schema(void)
{
    const char doc[] = "{\"schema_version\":1,\"product\":\"lamp\","
        "\"hardware_revision\":\"r2\",\"serial\":\"s-01\","
        "\"thingsboard_name\":\"device-01\"}";
    hq_device_info_t info;
    TEST_ASSERT_TRUE(osal_file_mock_add_file("/custom/device.json", doc));
    TEST_ASSERT_EQUAL_INT(HQ_JSON_DOC_OK,
        hq_device_info_load("/custom/device.json", &info));
    TEST_ASSERT_EQUAL_STRING("lamp", info.product);
    TEST_ASSERT_EQUAL_STRING("s-01", info.serial);

    TEST_ASSERT_TRUE(osal_file_mock_add_file("/custom/bad.json",
        "{\"schema_version\":1,\"product\":\"lamp\","
        "\"hardware_revision\":\"r2\",\"serial\":\"s-01\","
        "\"thingsboard_name\":\"device-01\",\"extra\":true}"));
    TEST_ASSERT_EQUAL_INT(HQ_JSON_DOC_ERR_MALFORMED,
        hq_device_info_load("/custom/bad.json", &info));
}

static void test_manufacturing_state_is_reported_without_policy_action(void)
{
    const char doc[] = "{\"schema_version\":1,\"manufacturing_state\":1,"
                       "\"credential_mode\":2}";
    hq_device_manufacturing_info_t manufacturing;
    TEST_ASSERT_TRUE(osal_file_mock_add_file("/factory/mfg.json", doc));
    TEST_ASSERT_EQUAL_INT(HQ_JSON_DOC_OK,
        hq_device_manufacturing_load("/factory/mfg.json", &manufacturing));
    TEST_ASSERT_TRUE(hq_device_info_is_provisioned(&manufacturing));
    manufacturing.credential_mode = HQ_DEVICE_CREDENTIAL_NONE;
    TEST_ASSERT_FALSE(hq_device_info_is_provisioned(&manufacturing));
}

static void test_tb_identity_loads_and_zeroizes_on_clear(void)
{
    const char doc[] = "{\"schema_version\":1,\"client_id\":\"lamp-01\","
                       "\"access_token\":\"credential-value\"}";
    TEST_ASSERT_TRUE(osal_file_mock_add_file("/tenant/identity.json", doc));
    TEST_ASSERT_EQUAL_INT(TB_IDENTITY_OK,
        tb_identity_load("/tenant/identity.json"));
    TEST_ASSERT_TRUE(tb_identity_is_loaded());
    const tb_identity_credentials_t *credentials = tb_identity_get();
    TEST_ASSERT_NOT_NULL(credentials);
    TEST_ASSERT_EQUAL_STRING("lamp-01", credentials->client_id);
    TEST_ASSERT_EQUAL_STRING("credential-value", credentials->access_token);
    tb_identity_clear();
    TEST_ASSERT_FALSE(tb_identity_is_loaded());
    TEST_ASSERT_NULL(tb_identity_get());
}

static void test_tb_identity_rejects_duplicate_members(void)
{
    const char doc[] = "{\"schema_version\":1,\"client_id\":\"lamp-01\","
        "\"access_token\":\"first\",\"access_token\":\"second\"}";
    TEST_ASSERT_TRUE(osal_file_mock_add_file("/tenant/identity.json", doc));
    TEST_ASSERT_EQUAL_INT(TB_IDENTITY_ERR_MALFORMED,
        tb_identity_load("/tenant/identity.json"));
    TEST_ASSERT_FALSE(tb_identity_is_loaded());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_strict_json_rejects_duplicate_and_nul_escape);
    RUN_TEST(test_device_info_uses_caller_path_and_strict_schema);
    RUN_TEST(test_manufacturing_state_is_reported_without_policy_action);
    RUN_TEST(test_tb_identity_loads_and_zeroizes_on_clear);
    RUN_TEST(test_tb_identity_rejects_duplicate_members);
    RUN_TEST(test_app_state_callbacks_are_synchronous_and_ordered);
    RUN_TEST(test_app_state_retry_returns_to_failed_stage_after_backoff);
    return UNITY_END();
}