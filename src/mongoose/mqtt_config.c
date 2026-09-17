#include "mqtt_config.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "hq_config.h"
#include "osal_file.h"
#include "osal_log.h"

#define MQTT_CONFIG_MAX_JSON_SIZE	4096

#ifndef CONFIG_MQTT_DEFAULT_ADDRESS
#define CONFIG_MQTT_DEFAULT_ADDRESS	"mqtt://192.168.1.169:1883"
#endif

#ifndef CONFIG_MQTT_DEFAULT_TOPIC_PREFIX
#define CONFIG_MQTT_DEFAULT_TOPIC_PREFIX	"/config/"
#endif

#ifndef CONFIG_MQTT_DEFAULT_POST_TOPIC
#define CONFIG_MQTT_DEFAULT_POST_TOPIC	"/post_data/"
#endif

#ifndef CONFIG_MQTT_DEFAULT_USERNAME
#define CONFIG_MQTT_DEFAULT_USERNAME	""
#endif

#ifndef CONFIG_MQTT_DEFAULT_PASSWORD
#define CONFIG_MQTT_DEFAULT_PASSWORD	""
#endif

#ifndef CONFIG_MQTT_DEFAULT_CLIENT_ID
#define CONFIG_MQTT_DEFAULT_CLIENT_ID	"hq_"
#endif

#ifndef CONFIG_MQTT_DEFAULT_SSL
#define CONFIG_MQTT_DEFAULT_SSL	0
#endif

typedef struct {
	mqtt_cert_source_t source;
	char value[MQTT_CONFIG_STR_SIZE];
	char resolved[MQTT_CERT_MAX_SIZE];
} cert_entry_t;

typedef struct {
	char address[MQTT_CONFIG_STR_SIZE];
	char topic_prefix[MQTT_CONFIG_STR_SIZE];
	char post_data_topic[MQTT_CONFIG_STR_SIZE];
	char username[MQTT_CONFIG_STR_SIZE];
	char password[MQTT_CONFIG_STR_SIZE];
	char client_id[MQTT_CONFIG_STR_SIZE];
	cert_entry_t cert;
	cert_entry_t client_cert;
	cert_entry_t client_key;
	uint8_t use_ssl;
	uint8_t skip_verify;
} config_data_t;

static mqtt_apply_config_cb apply_cb;
static config_data_t config;
static bool config_initialized;

static void str_copy_safe(char *dst, size_t dst_size, const char *src)
{
	if (!dst || dst_size == 0)
		return;

	if (!src) {
		dst[0] = '\0';
		return;
	}

	strncpy(dst, src, dst_size - 1);
	dst[dst_size - 1] = '\0';
}

static void set_defaults(void)
{
	memset(&config, 0, sizeof(config));
	str_copy_safe(config.address, sizeof(config.address),
		      CONFIG_MQTT_DEFAULT_ADDRESS);
	str_copy_safe(config.topic_prefix, sizeof(config.topic_prefix),
		      CONFIG_MQTT_DEFAULT_TOPIC_PREFIX);
	str_copy_safe(config.post_data_topic, sizeof(config.post_data_topic),
		      CONFIG_MQTT_DEFAULT_POST_TOPIC);
	str_copy_safe(config.username, sizeof(config.username),
		      CONFIG_MQTT_DEFAULT_USERNAME);
	str_copy_safe(config.password, sizeof(config.password),
		      CONFIG_MQTT_DEFAULT_PASSWORD);
	str_copy_safe(config.client_id, sizeof(config.client_id),
		      CONFIG_MQTT_DEFAULT_CLIENT_ID);
	config.use_ssl = CONFIG_MQTT_DEFAULT_SSL ? 1u : 0u;
	config.skip_verify = 0;
}

static bool read_file_to_buf(const char *path, char **out_buf)
{
	osal_fstat_t st = { 0 };
	osal_file_id_t fd;
	char *buffer;
	int32_t n;

	if (!path || !out_buf)
		return false;

	*out_buf = NULL;

	if (osal_stat(path, &st) != OSAL_SUCCESS)
		return false;

	if (st.file_size == 0 || st.file_size > MQTT_CONFIG_MAX_JSON_SIZE)
		return false;

	fd = osal_open_create(path, OSAL_FILE_FLAG_NONE, OSAL_READ_ONLY);
	if (fd < 0)
		return false;

	buffer = (char *)calloc(1u, st.file_size + 1u);
	if (!buffer) {
		(void)osal_close(fd);
		return false;
	}

	n = osal_read(fd, buffer, st.file_size);
	(void)osal_close(fd);

	if (n < 0) {
		free(buffer);
		return false;
	}

	buffer[n] = '\0';
	*out_buf = buffer;
	return true;
}

static bool write_file_from_buf(const char *path, const char *data, size_t len)
{
	osal_file_id_t fd;
	int32_t n;

	fd = osal_open_create(
		path, OSAL_FILE_FLAG_CREATE | OSAL_FILE_FLAG_TRUNCATE,
		OSAL_WRITE_ONLY);
	if (fd < 0)
		return false;

	n = osal_write(fd, data, len);
	(void)osal_close(fd);
	return n >= 0 && (size_t)n == len;
}

static const char *source_to_str(mqtt_cert_source_t source)
{
	switch (source) {
	case MQTT_CERT_SOURCE_FILE_PATH:
		return "file_path";
	case MQTT_CERT_SOURCE_RAW:
		return "raw";
	default:
		return "none";
	}
}

static mqtt_cert_source_t str_to_source(const char *str)
{
	if (!str)
		return MQTT_CERT_SOURCE_NONE;
	if (strcmp(str, "file_path") == 0)
		return MQTT_CERT_SOURCE_FILE_PATH;
	if (strcmp(str, "raw") == 0)
		return MQTT_CERT_SOURCE_RAW;
	return MQTT_CERT_SOURCE_NONE;
}

static bool resolve_cert_entry(cert_entry_t *entry)
{
	osal_fstat_t st = { 0 };
	osal_file_id_t fd;
	size_t to_read;
	int32_t n;

	entry->resolved[0] = '\0';

	switch (entry->source) {
	case MQTT_CERT_SOURCE_FILE_PATH:
		if (entry->value[0] == '\0')
			return false;

		if (osal_stat(entry->value, &st) != OSAL_SUCCESS ||
		    st.file_size == 0 || st.file_size >= sizeof(entry->resolved))
			return false;

		fd = osal_open_create(entry->value, OSAL_FILE_FLAG_NONE,
				      OSAL_READ_ONLY);
		if (fd < 0)
			return false;

		to_read = st.file_size < sizeof(entry->resolved) - 1
				  ? st.file_size
				  : sizeof(entry->resolved) - 1;
		n = osal_read(fd, entry->resolved, to_read);
		(void)osal_close(fd);

		if (n < 0 || (size_t)n != to_read) {
			entry->resolved[0] = '\0';
			return false;
		}
		entry->resolved[n] = '\0';
		return true;

	case MQTT_CERT_SOURCE_RAW:
		str_copy_safe(entry->resolved, sizeof(entry->resolved),
			      entry->value);
		return entry->value[0] != '\0';

	default:
		return false;
	}
}

static cert_entry_t *get_cert_entry(mqtt_config_value_t key)
{
	switch (key) {
	case MQTT_CONFIG_VALUE_CERT:
		return &config.cert;
	case MQTT_CONFIG_VALUE_CLIENT_CERT:
		return &config.client_cert;
	case MQTT_CONFIG_VALUE_CLIENT_KEY:
		return &config.client_key;
	default:
		return NULL;
	}
}

static void load_cert_from_json(cJSON *root, const char *field_name,
				cert_entry_t *entry)
{
	cJSON *obj;
	cJSON *source_item;
	cJSON *value_item;

	obj = cJSON_GetObjectItemCaseSensitive(root, field_name);
	if (!cJSON_IsObject(obj))
		return;

	source_item = cJSON_GetObjectItemCaseSensitive(obj, "source");
	value_item = cJSON_GetObjectItemCaseSensitive(obj, "value");

	if (!cJSON_IsString(source_item) || !source_item->valuestring)
		return;

	entry->source = str_to_source(source_item->valuestring);

	if (cJSON_IsString(value_item) && value_item->valuestring)
		str_copy_safe(entry->value, sizeof(entry->value),
			      value_item->valuestring);

	(void)resolve_cert_entry(entry);
}

static bool save_cert_to_json(cJSON *root, const char *field_name,
			      const cert_entry_t *entry)
{
	cJSON *obj;

	if (entry->source == MQTT_CERT_SOURCE_NONE)
		return true;

	obj = cJSON_CreateObject();
	if (!obj)
		return false;

	if (!cJSON_AddStringToObject(obj, "source",
				     source_to_str(entry->source))) {
		cJSON_Delete(obj);
		return false;
	}

	if (!cJSON_AddStringToObject(obj, "value", entry->value)) {
		cJSON_Delete(obj);
		return false;
	}

	if (!cJSON_AddItemToObject(root, field_name, obj)) {
		cJSON_Delete(obj);
		return false;
	}

	return true;
}

static bool load_json_config(void)
{
	cJSON *root;
	cJSON *item;
	char *content = NULL;

	if (!read_file_to_buf(MQTT_CONFIG_FILE_PATH, &content))
		return false;

	root = cJSON_Parse(content);
	free(content);
	if (!root)
		return false;

	item = cJSON_GetObjectItemCaseSensitive(root, "address");
	if (cJSON_IsString(item) && item->valuestring)
		str_copy_safe(config.address, sizeof(config.address),
			      item->valuestring);

	item = cJSON_GetObjectItemCaseSensitive(root, "ssl");
	if (cJSON_IsBool(item))
		config.use_ssl = cJSON_IsTrue(item) ? 1 : 0;

	item = cJSON_GetObjectItemCaseSensitive(root, "skip_verify");
	if (cJSON_IsBool(item))
		config.skip_verify = cJSON_IsTrue(item) ? 1 : 0;

	item = cJSON_GetObjectItemCaseSensitive(root, "prefix");
	if (cJSON_IsString(item) && item->valuestring)
		str_copy_safe(config.topic_prefix, sizeof(config.topic_prefix),
			      item->valuestring);

	item = cJSON_GetObjectItemCaseSensitive(root, "post");
	if (cJSON_IsString(item) && item->valuestring)
		str_copy_safe(config.post_data_topic,
			      sizeof(config.post_data_topic),
			      item->valuestring);

	item = cJSON_GetObjectItemCaseSensitive(root, "user");
	if (cJSON_IsString(item) && item->valuestring)
		str_copy_safe(config.username, sizeof(config.username),
			      item->valuestring);

	item = cJSON_GetObjectItemCaseSensitive(root, "pass");
	if (cJSON_IsString(item) && item->valuestring)
		str_copy_safe(config.password, sizeof(config.password),
			      item->valuestring);

	item = cJSON_GetObjectItemCaseSensitive(root, "client_id");
	if (cJSON_IsString(item) && item->valuestring)
		str_copy_safe(config.client_id, sizeof(config.client_id),
			      item->valuestring);

	load_cert_from_json(root, "cert", &config.cert);
	load_cert_from_json(root, "client_cert", &config.client_cert);
	load_cert_from_json(root, "client_key", &config.client_key);

	cJSON_Delete(root);
	return true;
}

static bool save_json_config(void)
{
	cJSON *root;
	char *json;
	bool ok;

	root = cJSON_CreateObject();
	if (!root)
		return false;

	ok = cJSON_AddStringToObject(root, "address", config.address) != NULL;
	ok = ok && cJSON_AddBoolToObject(root, "ssl",
					 config.use_ssl != 0) != NULL;
	ok = ok && cJSON_AddBoolToObject(root, "skip_verify",
					 config.skip_verify != 0) != NULL;
	ok = ok && cJSON_AddStringToObject(root, "prefix",
					   config.topic_prefix) != NULL;
	ok = ok && cJSON_AddStringToObject(root, "post",
					   config.post_data_topic) != NULL;
	ok = ok && cJSON_AddStringToObject(root, "user",
					   config.username) != NULL;
	ok = ok && cJSON_AddStringToObject(root, "pass",
					   config.password) != NULL;
	ok = ok && cJSON_AddStringToObject(root, "client_id",
					   config.client_id) != NULL;

	ok = ok && save_cert_to_json(root, "cert", &config.cert);
	ok = ok && save_cert_to_json(root, "client_cert", &config.client_cert);
	ok = ok && save_cert_to_json(root, "client_key", &config.client_key);

	if (!ok) {
		cJSON_Delete(root);
		return false;
	}

	json = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (!json)
		return false;

	ok = write_file_from_buf(MQTT_CONFIG_FILE_PATH, json, strlen(json));
	cJSON_free(json);
	return ok;
}

void mqtt_config_init(void)
{
	if (config_initialized)
		return;

	set_defaults();
	(void)load_json_config();
	config_initialized = true;
}

bool mqtt_config_set_bool(bool value, mqtt_config_value_t key)
{
	switch (key) {
	case MQTT_CONFIG_VALUE_SSL:
		config.use_ssl = value ? 1u : 0u;
		return true;
	case MQTT_CONFIG_VALUE_SKIP_VERIFY:
		config.skip_verify = value ? 1u : 0u;
		return true;
	default:
		return false;
	}
}

bool mqtt_config_set_cert_source(mqtt_cert_source_t source, const char *value,
				 mqtt_config_value_t key)
{
	cert_entry_t *entry;

	entry = get_cert_entry(key);
	if (!entry)
		return false;

	if (source == MQTT_CERT_SOURCE_NONE) {
		entry->source = MQTT_CERT_SOURCE_NONE;
		entry->value[0] = '\0';
		entry->resolved[0] = '\0';
		return true;
	}

	if (!value)
		return false;

	entry->source = source;
	str_copy_safe(entry->value, sizeof(entry->value), value);
	return resolve_cert_entry(entry);
}

bool mqtt_config_set_string(const char *string, mqtt_config_value_t key)
{
	if (!string)
		return false;

	switch (key) {
	case MQTT_CONFIG_VALUE_ADDRESS:
		str_copy_safe(config.address, sizeof(config.address), string);
		return true;
	case MQTT_CONFIG_VALUE_TOPIC_PREFIX:
		str_copy_safe(config.topic_prefix, sizeof(config.topic_prefix),
			      string);
		return true;
	case MQTT_CONFIG_VALUE_POST_DATA_TOPIC:
		str_copy_safe(config.post_data_topic,
			      sizeof(config.post_data_topic), string);
		return true;
	case MQTT_CONFIG_VALUE_USERNAME:
		str_copy_safe(config.username, sizeof(config.username), string);
		return true;
	case MQTT_CONFIG_VALUE_PASSWORD:
		str_copy_safe(config.password, sizeof(config.password), string);
		return true;
	case MQTT_CONFIG_VALUE_CLIENT_ID:
		str_copy_safe(config.client_id, sizeof(config.client_id),
			      string);
		return true;
	default:
		return false;
	}
}

bool mqtt_config_get_bool(bool *value, mqtt_config_value_t key)
{
	if (!value)
		return false;

	switch (key) {
	case MQTT_CONFIG_VALUE_SSL:
		*value = config.use_ssl != 0;
		return true;
	case MQTT_CONFIG_VALUE_SKIP_VERIFY:
		*value = config.skip_verify != 0;
		return true;
	default:
		return false;
	}
}

const char *mqtt_config_get_string(mqtt_config_value_t key)
{
	switch (key) {
	case MQTT_CONFIG_VALUE_ADDRESS:
		return config.address;
	case MQTT_CONFIG_VALUE_TOPIC_PREFIX:
		return config.topic_prefix;
	case MQTT_CONFIG_VALUE_POST_DATA_TOPIC:
		return config.post_data_topic;
	case MQTT_CONFIG_VALUE_USERNAME:
		return config.username;
	case MQTT_CONFIG_VALUE_PASSWORD:
		return config.password;
	case MQTT_CONFIG_VALUE_CLIENT_ID:
		return config.client_id;
	default:
		return NULL;
	}
}

const char *mqtt_config_get_cert(mqtt_config_value_t key)
{
	cert_entry_t *entry;

	entry = get_cert_entry(key);
	if (!entry)
		return NULL;

	return entry->resolved;
}

bool mqtt_config_get_cert_source(mqtt_cert_source_t *source, const char **value,
				 mqtt_config_value_t key)
{
	cert_entry_t *entry;

	entry = get_cert_entry(key);
	if (!entry)
		return false;

	if (source)
		*source = entry->source;
	if (value)
		*value = entry->value;
	return true;
}

bool mqtt_config_save(void)
{
	bool ok;

	ok = save_json_config();
	if (!ok) {
		osal_log_error("mqtt config save failed");
		return false;
	}

	if (apply_cb)
		apply_cb();

	return true;
}

void mqtt_config_set_callback(mqtt_apply_config_cb cb)
{
	apply_cb = cb;
}
