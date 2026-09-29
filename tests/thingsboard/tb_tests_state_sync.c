#include "tb_test_common.h"

#include "tb_state_sync.h"

#define ATTR_RESPONSE_PREFIX "v1/devices/me/attributes/response/"
#define ATTR_REQUEST_PREFIX "v1/devices/me/attributes/request/"

typedef struct test_state {
    uint32_t value;
    uint64_t wide; /* Requires 8-byte alignment of engine buffers. */
} test_state_t;

static void assert_state_aligned(const void *state)
{
    TEST_ASSERT_EQUAL_UINT(0u, (unsigned)((uintptr_t)state % sizeof(uint64_t)));
}

typedef struct test_state_sync_context {
    tb_client_t *client;
    test_state_t applied;
    unsigned apply_count;
    unsigned inactive_count;
    unsigned telemetry_count;
    unsigned rpc_count;
    bool last_apply_suspended;
} test_state_sync_context_t;

static uint32_t s_state_sync_now;
static test_state_sync_context_t s_state_sync_context;

static uint32_t state_sync_now(void)
{
    return s_state_sync_now;
}

static tb_state_sync_parse_result_t parse_test_state(
    const char *json, tb_state_sync_source_t source, void *state_out,
    void *user_data)
{
    cJSON *root;
    cJSON *scope;
    cJSON *value;
    (void)user_data;

    if (json == NULL || state_out == NULL) {
        return TB_STATE_SYNC_PARSE_INVALID;
    }
    assert_state_aligned(state_out);
    root = cJSON_Parse(json);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return TB_STATE_SYNC_PARSE_INVALID;
    }
    if (source == TB_STATE_SYNC_SOURCE_DEFAULT) {
        bool empty = cJSON_GetArraySize(root) == 0;
        cJSON_Delete(root);
        if (empty) {
            ((test_state_t *)state_out)->value = 3u;
            return TB_STATE_SYNC_PARSE_VALID;
        }
        return TB_STATE_SYNC_PARSE_INVALID;
    }
    if (source == TB_STATE_SYNC_SOURCE_CLIENT ||
        source == TB_STATE_SYNC_SOURCE_SHARED) {
        scope = cJSON_GetObjectItemCaseSensitive(
            root, source == TB_STATE_SYNC_SOURCE_CLIENT ? "client" : "shared");
    } else {
        scope = root;
    }
    value = cJSON_GetObjectItemCaseSensitive(scope, "value");
    if (!cJSON_IsNumber(value) || value->valueint < 0 ||
        (double)value->valueint != value->valuedouble) {
        cJSON_Delete(root);
        return TB_STATE_SYNC_PARSE_INVALID;
    }
    ((test_state_t *)state_out)->value = (uint32_t)value->valueint;
    cJSON_Delete(root);
    return TB_STATE_SYNC_PARSE_VALID;
}

static bool apply_test_state(const void *state, bool output_suspended,
                             void *user_data)
{
    test_state_sync_context_t *context = user_data;
    assert_state_aligned(state);
    context->applied = *(const test_state_t *)state;
    context->apply_count++;
    context->last_apply_suspended = output_suspended;
    return true;
}

static bool test_states_equal(const void *left, const void *right,
                              void *user_data)
{
    (void)user_data;
    return ((const test_state_t *)left)->value ==
           ((const test_state_t *)right)->value;
}

static void force_test_inactive(void *user_data)
{
    ((test_state_sync_context_t *)user_data)->inactive_count++;
}

static void publish_test_attributes(const void *state, void *user_data)
{
    (void)state;
    (void)user_data;
}

static void publish_test_telemetry(void *user_data)
{
    ((test_state_sync_context_t *)user_data)->telemetry_count++;
}

static void handle_test_rpc(const char *method, const char *params_json,
                            uint32_t request_id, void *user_data)
{
    test_state_sync_context_t *context = user_data;
    (void)params_json;
    context->rpc_count++;
    TEST_ASSERT_EQUAL_STRING("ping", method);
    TEST_ASSERT_EQUAL_INT(0, tb_rpc_respond(context->client, request_id, "{}"));
}

static void test_state_sync_transport_and_dispatch(void)
{
    static const char *const client_keys[] = { "value" };
    static const char *const shared_keys[] = { "value" };
    test_state_sync_context_t context = { 0 };
    tb_state_sync_config_t config = { 0 };
    tb_client_t *client;
    int request_index;
    uint32_t request_id;
    char response_topic[128];
    test_state_t applied = { 0 };
    bool has_state = false;

    s_state_sync_now = 1000u;
    s_state_sync_context = context;
    client = create_test_client();
    TEST_ASSERT_NOT_NULL(client);
    s_state_sync_context.client = client;

    config.client = client;
    config.client_keys = client_keys;
    config.client_key_count = 1u;
    config.shared_keys = shared_keys;
    config.shared_key_count = 1u;
    config.max_payload_bytes = 512u;
    config.state_size = sizeof(test_state_t);
    config.sync_timeout_ms = 1000u;
    config.retry_initial_delay_ms = 100u;
    config.retry_max_delay_ms = 400u;
    config.telemetry_period_ms = 30000u;
    config.now_ms = state_sync_now;
    config.parse_state = parse_test_state;
    config.apply_state = apply_test_state;
    config.states_equal = test_states_equal;
    config.force_inactive = force_test_inactive;
    config.publish_applied_attributes = publish_test_attributes;
    config.publish_telemetry = publish_test_telemetry;
    config.user_data = &s_state_sync_context;
    TEST_ASSERT_EQUAL(TB_STATE_SYNC_OK, tb_state_sync_init(&config));
    TEST_ASSERT_EQUAL_INT(0, tb_state_sync_register_rpc_handler(
        "ping", handle_test_rpc, &s_state_sync_context));

    tb_state_sync_on_connected(client);
    request_index = find_last_publish_with_prefix(ATTR_REQUEST_PREFIX);
    TEST_ASSERT_TRUE(request_index >= 0);
    request_id = parse_topic_suffix_id(mock_publishes[request_index].topic);
    snprintf(response_topic, sizeof(response_topic), "%s%u",
             ATTR_RESPONSE_PREFIX, (unsigned)request_id);
    mqtt_app_mock_deliver_message(
        response_topic,
        "{\"client\":{\"value\":17},\"shared\":{\"value\":29}}",
        strlen("{\"client\":{\"value\":17},\"shared\":{\"value\":29}}"));
    TEST_ASSERT_EQUAL_UINT32(17u, s_state_sync_context.applied.value);
    TEST_ASSERT_EQUAL_UINT32(1u, s_state_sync_context.apply_count);
    TEST_ASSERT_EQUAL_UINT32(2u, s_state_sync_context.telemetry_count);
    TEST_ASSERT_TRUE(tb_state_sync_is_synchronized(client));
    TEST_ASSERT_EQUAL(TB_STATE_SYNC_OK,
                      tb_state_sync_get_state(client, &has_state, &applied));
    TEST_ASSERT_TRUE(has_state);
    TEST_ASSERT_EQUAL_UINT32(17u, applied.value);

    mqtt_app_mock_deliver_message("v1/devices/me/attributes",
                                  "{\"value\":23}",
                                  strlen("{\"value\":23}"));
    TEST_ASSERT_EQUAL_UINT32(23u, s_state_sync_context.applied.value);
    TEST_ASSERT_EQUAL_UINT32(2u, s_state_sync_context.apply_count);

    mqtt_app_mock_deliver_message("v1/devices/me/rpc/request/41",
                                  "{\"method\":\"ping\",\"params\":{}}",
                                  strlen("{\"method\":\"ping\",\"params\":{}}"));
    TEST_ASSERT_EQUAL_UINT32(1u, s_state_sync_context.rpc_count);

    tb_state_sync_deinit();
    mqtt_app_mock_deliver_message("v1/devices/me/attributes",
                                  "{\"value\":31}",
                                  strlen("{\"value\":31}"));
    TEST_ASSERT_EQUAL_UINT32(2u, s_state_sync_context.apply_count);
    destroy_test_client(client);
}

void run_state_sync_tests(void)
{
    RUN_TEST(test_state_sync_transport_and_dispatch);
}