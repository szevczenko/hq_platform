/*
 * Wi-Fi HTTP provisioning callback subscription unit tests (POSIX).
 *
 * Focuses on the lifecycle's Wi-Fi event subscribe/unsubscribe symmetry.  The
 * provisioning application subscribes four typed Wi-Fi events when it starts
 * (CONNECTED, DISCONNECTED, CONNECT_FAILED, MODE_CHANGED) to feed the status
 * route, and must unsubscribe every one of them when it stops.  Repeated
 * start/stop cycles and bind-failure rollbacks must never leak a subscription
 * or leave an armed Wi-Fi event callback behind.
 *
 * The provisioning module is compiled directly against a lightweight mock of
 * the Wi-Fi management API that counts live subscriptions, while Mongoose and
 * the captive DNS service are the real ones used by the application.  This is
 * deliberately deterministic: no real driver state is involved, so the number
 * of live callbacks is asserted exactly.
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "captive_dns_server.h"
#include "mongoose_process.h"
#include "osal_task.h"
#include "unity.h"
#include "wifi_http_provisioning.h"
#include "wifi_managment.h"

#ifdef ESP_PLATFORM
#error "wifi_http_provisioning_subscription_test.c targets POSIX only"
#endif

/* Live Wi-Fi event subscription bookkeeping.                               */
#define SUB_MAX 16
typedef struct {
	wifi_mgmt_event_t    event;
	wifi_mgmt_event_cb_t cb;
	void               *user;
} mock_sub_t;

static mock_sub_t s_subs[SUB_MAX];
static int        s_sub_count   = 0;
static bool       s_wifi_running = true;

/* Mock of the Wi-Fi management API --------------------------------------- */

bool wifi_mgmt_is_running(void) { return s_wifi_running; }
bool wifi_mgmt_trying_connect(void) { return false; }
bool wifi_mgmt_is_connected(void) { return true; }

bool wifi_mgmt_get_ip_info(wifi_mgmt_ip_info_t *info)
{
	if (info == NULL) return false;
	strncpy(info->ssid, "mock", sizeof(info->ssid) - 1);
	info->ssid[sizeof(info->ssid) - 1] = '\0';
	strncpy(info->ip, "192.168.1.5", sizeof(info->ip) - 1);
	info->ip[sizeof(info->ip) - 1] = '\0';
	strncpy(info->netmask, "255.255.255.0", sizeof(info->netmask) - 1);
	info->netmask[sizeof(info->netmask) - 1] = '\0';
	strncpy(info->gw, "192.168.1.1", sizeof(info->gw) - 1);
	info->gw[sizeof(info->gw) - 1] = '\0';
	info->urc = 0;
	return true;
}

bool wifi_mgmt_get_access_points(wifi_mgmt_ap_list_t *list)
{
	if (list != NULL) list->count = 0;
	return true;
}

uint32_t wifi_mgmt_get_scan_generation(void) { return 0u; }
bool wifi_mgmt_is_scan_active(void) { return false; }
bool wifi_mgmt_start_scan_no_block(void) { return true; }
bool wifi_mgmt_connect(void) { return true; }
bool wifi_mgmt_disconnect(void) { return true; }
bool wifi_mgmt_request_mode(wifi_type_t type) { (void)type; return true; }
bool wifi_mgmt_set_ap_name(const char *name, size_t len) { (void)name; (void)len; return true; }
bool wifi_mgmt_set_password(const char *passwd, size_t len) { (void)passwd; (void)len; return true; }

bool wifi_mgmt_subscribe(wifi_mgmt_event_t event, wifi_mgmt_event_cb_t cb, void *user)
{
	int i;
	if (cb == NULL) return false;
	for (i = 0; i < s_sub_count; ++i) {
		if (s_subs[i].event == event && s_subs[i].cb == cb &&
		    s_subs[i].user == user)
			return false;			/* duplicate. */
	}
	if (s_sub_count >= SUB_MAX) return false;
	s_subs[s_sub_count].event = event;
	s_subs[s_sub_count].cb    = cb;
	s_subs[s_sub_count].user  = user;
	++s_sub_count;
	return true;
}

bool wifi_mgmt_unsubscribe(wifi_mgmt_event_t event, wifi_mgmt_event_cb_t cb, void *user)
{
	int i;
	if (cb == NULL) return false;
	for (i = 0; i < s_sub_count; ++i) {
		if (s_subs[i].event == event && s_subs[i].cb == cb &&
		    s_subs[i].user == user) {
			for (; i + 1 < s_sub_count; ++i) s_subs[i] = s_subs[i + 1];
			--s_sub_count;
			return true;
		}
	}
	return false;
}

/* -- socket helpers (real bind conflicts drive the rollback paths) -------- */

static int free_tcp_port(void)
{
	struct sockaddr_in addr;
	socklen_t          len = sizeof(addr);
	int                fd  = (int)socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) return -1;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family      = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port        = 0;
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) { close(fd); return -1; }
	if (getsockname(fd, (struct sockaddr *)&addr, &len) != 0) { close(fd); return -1; }
	int port = ntohs(addr.sin_port);
	close(fd);
	return port;
}

static int free_udp_port(void)
{
	struct sockaddr_in addr;
	socklen_t          len = sizeof(addr);
	int                fd  = (int)socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) return -1;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family      = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port        = 0;
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) { close(fd); return -1; }
	if (getsockname(fd, (struct sockaddr *)&addr, &len) != 0) { close(fd); return -1; }
	int port = ntohs(addr.sin_port);
	close(fd);
	return port;
}

static int open_conflict_tcp(int *port)
{
	struct sockaddr_in addr;
	socklen_t          len = sizeof(addr);
	int                fd  = (int)socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) return -1;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family      = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port        = 0;
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) { close(fd); return -2; }
	if (getsockname(fd, (struct sockaddr *)&addr, &len) != 0) { close(fd); return -3; }
	*port = ntohs(addr.sin_port);
	if (listen(fd, 4) != 0) { close(fd); return -4; }
	return fd;
}

static int open_conflict_udp(int *port)
{
	struct sockaddr_in addr;
	socklen_t          len = sizeof(addr);
	int                fd  = (int)socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) return -1;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family      = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port        = 0;
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) { close(fd); return -2; }
	if (getsockname(fd, (struct sockaddr *)&addr, &len) != 0) { close(fd); return -3; }
	*port = ntohs(addr.sin_port);
	return fd;
}

static void build_http_url(char *out, size_t cap, int port)
{
	snprintf(out, cap, "http://127.0.0.1:%d", port);
}

static void build_dns_url(char *out, size_t cap, int port)
{
	snprintf(out, cap, "udp://127.0.0.1:%d", port);
}

static void configure_fresh_listeners(char *http_url, size_t http_cap,
                                      char *dns_url, size_t dns_cap)
{
	int http_port = free_tcp_port();
	int dns_port  = free_udp_port();
	TEST_ASSERT_TRUE(http_port > 0);
	TEST_ASSERT_TRUE(dns_port > 0);
	build_http_url(http_url, http_cap, http_port);
	build_dns_url(dns_url, dns_cap, dns_port);
	TEST_ASSERT_TRUE(wifi_http_provisioning_set_http_url(http_url));
	TEST_ASSERT_TRUE(wifi_http_provisioning_set_dns_url(dns_url));
}

static int sub_count_for(wifi_mgmt_event_t event)
{
	int n = 0, i;
	for (i = 0; i < s_sub_count; ++i) if (s_subs[i].event == event) ++n;
	return n;
}

static void reset_subs(void)
{
	s_sub_count = 0;
	s_wifi_running = true;
	memset(s_subs, 0, sizeof(s_subs));
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

/* A clean start subscribes the status route's four Wi-Fi events; a clean stop
 * must unsubscribe all of them. */
static void test_clean_start_subscribes_stop_unsubscribes(void)
{
	char http_url[64], dns_url[64];

	configure_fresh_listeners(http_url, sizeof(http_url), dns_url, sizeof(dns_url));

	TEST_ASSERT_TRUE(wifi_http_provisioning_start());
	TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_RUNNING, wifi_http_provisioning_get_state());
	TEST_ASSERT_EQUAL_INT(4, s_sub_count);
	TEST_ASSERT_EQUAL_INT(1, sub_count_for(WIFI_MGMT_EVENT_CONNECTED));
	TEST_ASSERT_EQUAL_INT(1, sub_count_for(WIFI_MGMT_EVENT_DISCONNECTED));
	TEST_ASSERT_EQUAL_INT(1, sub_count_for(WIFI_MGMT_EVENT_CONNECT_FAILED));
	TEST_ASSERT_EQUAL_INT(1, sub_count_for(WIFI_MGMT_EVENT_MODE_CHANGED));

	TEST_ASSERT_TRUE(wifi_http_provisioning_stop());
	TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_STOPPED, wifi_http_provisioning_get_state());
	TEST_ASSERT_EQUAL_INT(0, s_sub_count);
}

/* Repeated start/stop cycles must never accumulate Wi-Fi event callbacks. */
static void test_repeated_cycles_do_not_leak_subscriptions(void)
{
	char http_url[64], dns_url[64];
	int  i;

	for (i = 0; i < 25; ++i) {
		configure_fresh_listeners(http_url, sizeof(http_url), dns_url, sizeof(dns_url));
		TEST_ASSERT_TRUE(wifi_http_provisioning_start());
		TEST_ASSERT_EQUAL_INT(4, s_sub_count);
		TEST_ASSERT_TRUE(wifi_http_provisioning_stop());
		TEST_ASSERT_EQUAL_INT(0, s_sub_count);
	}
}

/* HTTP bind failure rolls back the DNS listener; the status subscriptions
 * are only registered once the app reaches RUNNING, so a failed start must
 * leave zero callbacks live. */
static void test_http_bind_failure_leaves_no_subscriptions(void)
{
	char  http_url[64], dns_url[64];
	int   conflict_tcp, free_dns, fd;

	free_dns = free_udp_port();
	fd       = open_conflict_tcp(&conflict_tcp);
	TEST_ASSERT_TRUE(fd >= 0);
	TEST_ASSERT_TRUE(conflict_tcp > 0);
	TEST_ASSERT_TRUE(free_dns > 0);
	build_http_url(http_url, sizeof(http_url), conflict_tcp);
	build_dns_url(dns_url, sizeof(dns_url), free_dns);
	TEST_ASSERT_TRUE(wifi_http_provisioning_set_http_url(http_url));
	TEST_ASSERT_TRUE(wifi_http_provisioning_set_dns_url(dns_url));

	TEST_ASSERT_FALSE(wifi_http_provisioning_start());
	TEST_ASSERT_FALSE(captive_dns_server_is_running());
	TEST_ASSERT_EQUAL_INT(0, s_sub_count);
	close(fd);

	wifi_http_provisioning_stop();
	TEST_ASSERT_EQUAL_INT(0, s_sub_count);
}

/* DNS bind failure happens before any listener is up; nothing is subscribed. */
static void test_dns_bind_failure_leaves_no_subscriptions(void)
{
	char http_url[64], dns_url[64];
	int  free_tcp, conflict_dns, fd;

	free_tcp = free_tcp_port();
	fd       = open_conflict_udp(&conflict_dns);
	TEST_ASSERT_TRUE(fd >= 0);
	TEST_ASSERT_TRUE(conflict_dns > 0);
	TEST_ASSERT_TRUE(free_tcp > 0);
	build_http_url(http_url, sizeof(http_url), free_tcp);
	build_dns_url(dns_url, sizeof(dns_url), conflict_dns);
	TEST_ASSERT_TRUE(wifi_http_provisioning_set_http_url(http_url));
	TEST_ASSERT_TRUE(wifi_http_provisioning_set_dns_url(dns_url));

	TEST_ASSERT_FALSE(wifi_http_provisioning_start());
	TEST_ASSERT_FALSE(captive_dns_server_is_running());
	TEST_ASSERT_EQUAL_INT(0, s_sub_count);
	close(fd);

	wifi_http_provisioning_stop();
	TEST_ASSERT_EQUAL_INT(0, s_sub_count);
}

/* With Wi-Fi management stopped, start fails cleanly and subscribes nothing. */
static void test_wifi_down_subscribes_nothing(void)
{
	char http_url[64], dns_url[64];

	s_wifi_running = false;
	configure_fresh_listeners(http_url, sizeof(http_url), dns_url, sizeof(dns_url));

	TEST_ASSERT_FALSE(wifi_http_provisioning_start());
	TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_ERROR, wifi_http_provisioning_get_state());
	TEST_ASSERT_EQUAL_INT(0, s_sub_count);

	wifi_http_provisioning_stop();
	s_wifi_running = true;
}

/* ------------------------------------------------------------------ */
/* Setup / teardown                                                    */
/* ------------------------------------------------------------------ */

void setUp(void)
{
	reset_subs();
}

void tearDown(void)
{
	wifi_http_provisioning_stop();
	reset_subs();
}

int main(void)
{
	int rc = 0;

	/* Shared Mongoose process for listener bring-up. */
	MongooseProcess_Deinit();
	MongooseProcess_Init();
	TEST_ASSERT_TRUE(MongooseProcess_IsRunning());

	setvbuf(stdout, NULL, _IONBF, 0);
	UNITY_BEGIN();

	RUN_TEST(test_clean_start_subscribes_stop_unsubscribes);
	RUN_TEST(test_repeated_cycles_do_not_leak_subscriptions);
	RUN_TEST(test_http_bind_failure_leaves_no_subscriptions);
	RUN_TEST(test_dns_bind_failure_leaves_no_subscriptions);
	RUN_TEST(test_wifi_down_subscribes_nothing);

	/* Teardown must leave the shared process (and its listeners) clean. */
	wifi_http_provisioning_stop();
	TEST_ASSERT_FALSE(captive_dns_server_is_running());
	TEST_ASSERT_EQUAL_INT(0, s_sub_count);
	TEST_ASSERT_TRUE(MongooseProcess_IsRunning());
	MongooseProcess_Deinit();

	rc = UNITY_END();
	return rc;
}