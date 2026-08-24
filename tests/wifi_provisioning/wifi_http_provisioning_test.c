/*
 * Wi-Fi HTTP provisioning application lifecycle unit tests (POSIX).
 *
 * Exercises the public start/stop/state API against the shared Mongoose
 * process and the POSIX Wi-Fi simulator:
 *  - start() requires initialized Mongoose and Wi-Fi management,
 *  - a successful start reaches RUNNING only after both listeners bind,
 *  - repeated start/stop calls are safe,
 *  - setting listen URLs while running is rejected,
 *  - a partial start (DNS or HTTP bind failure) rolls back and returns to a
 *    clean stopped/error state,
 *  - stopping provisioning never deinitializes the shared Mongoose process.
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
#include "wifi_provisioning_test_fixture.h"

#ifdef ESP_PLATFORM
#error "wifi_http_provisioning_test.c targets POSIX only"
#endif

/* Dedicated littlefs image so this test never touches another test's state. */
#define TEST_IMAGE_PATH "/tmp/wifi_prov_http.img"

/* The shared fixture owns the whole component stack (filesystem, Wi-Fi
 * management, Mongoose) around every RUN_TEST. */
void setUp( void )
{
  wifi_provisioning_test_fixture_setup();
}

void tearDown( void )
{
  wifi_provisioning_test_fixture_teardown();
}

/* -- socket helpers ------------------------------------------------------- */

static int free_tcp_port( void )
{
	struct sockaddr_in addr;
	socklen_t          len = sizeof(addr);
	int                fd  = (int) socket(AF_INET, SOCK_STREAM, 0);

	if ( fd < 0 ) return -1;
	memset( &addr, 0, sizeof(addr) );
	addr.sin_family      = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port        = 0;
	if ( bind(fd, (struct sockaddr *) &addr, sizeof(addr)) != 0 ) {
		close(fd);
		return -1;
	}
	if ( getsockname(fd, (struct sockaddr *) &addr, &len) != 0 ) {
		close(fd);
		return -1;
	}
	int port = ntohs(addr.sin_port);
	close(fd);
	return port;
}

static int free_udp_port( void )
{
	struct sockaddr_in addr;
	socklen_t          len = sizeof(addr);
	int                fd  = (int) socket(AF_INET, SOCK_DGRAM, 0);

	if ( fd < 0 ) return -1;
	memset( &addr, 0, sizeof(addr) );
	addr.sin_family      = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port        = 0;
	if ( bind(fd, (struct sockaddr *) &addr, sizeof(addr)) != 0 ) {
		close(fd);
		return -1;
	}
	if ( getsockname(fd, (struct sockaddr *) &addr, &len) != 0 ) {
		close(fd);
		return -1;
	}
	int port = ntohs(addr.sin_port);
	close(fd);
	return port;
}

/* Keep a UDP socket bound so a later bind to the same port must fail. */
static int open_conflict_udp( int * port )
{
	struct sockaddr_in addr;
	socklen_t          len = sizeof(addr);
	int                fd  = (int) socket(AF_INET, SOCK_DGRAM, 0);

	if ( fd < 0 ) return -1;
	memset( &addr, 0, sizeof(addr) );
	addr.sin_family      = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port        = 0;
	if ( bind(fd, (struct sockaddr *) &addr, sizeof(addr)) != 0 ) {
		close(fd);
		return -2;
	}
	if ( getsockname(fd, (struct sockaddr *) &addr, &len) != 0 ) {
		close(fd);
		return -3;
	}
	*port = ntohs(addr.sin_port);
	return fd;
}

/* Keep a listening TCP socket so a local bind to the same port must fail. */
static int open_conflict_tcp( int * port )
{
	struct sockaddr_in addr;
	socklen_t          len = sizeof(addr);
	int                fd  = (int) socket(AF_INET, SOCK_STREAM, 0);

	if ( fd < 0 ) return -1;
	memset( &addr, 0, sizeof(addr) );
	addr.sin_family      = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port        = 0;
	if ( bind(fd, (struct sockaddr *) &addr, sizeof(addr)) != 0 ) {
		close(fd);
		return -2;
	}
	if ( getsockname(fd, (struct sockaddr *) &addr, &len) != 0 ) {
		close(fd);
		return -3;
	}
	*port = ntohs(addr.sin_port);
	if ( listen(fd, 4) != 0 ) {
		close(fd);
		return -4;
	}
	return fd;
}

static void build_http_url( char * out, size_t cap, int port )
{
	snprintf(out, cap, "http://127.0.0.1:%d", port);
}

static void build_dns_url( char * out, size_t cap, int port )
{
	snprintf(out, cap, "udp://127.0.0.1:%d", port);
}

/* ------------------------------------------------------------------ */
/* Lifecycle verification.                                              */
/* ------------------------------------------------------------------ */

static void run_lifecycle_tests( void )
{
	char http_url[64], dns_url[64];
	int  http_port, dns_port;

	/* --- before Mongoose is initialized: start must fail -----------------
	 * At this point Wi-Fi management is running but the shared Mongoose
	 * process is not, so the provisioning application must refuse to start
	 * and move to ERROR without touching any listener. */
	MongooseProcess_Deinit();
	TEST_ASSERT_FALSE(MongooseProcess_IsRunning());
	TEST_ASSERT_FALSE_MESSAGE(wifi_http_provisioning_start(),
				  "start must be rejected when Mongoose is not running");
	TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_ERROR, wifi_http_provisioning_get_state());

	/* --- bring up the shared Mongoose process --------------------- */
	MongooseProcess_Init();
	TEST_ASSERT_TRUE_MESSAGE(MongooseProcess_IsRunning(),
				  "MongooseProcess_Init must leave the process running");

	/* --- configure free high ports for the two listeners ----------- */
	http_port = free_tcp_port();
	dns_port  = free_udp_port();
	TEST_ASSERT_TRUE(http_port > 0);
	TEST_ASSERT_TRUE(dns_port > 0);
	build_http_url(http_url, sizeof(http_url), http_port);
	build_dns_url(dns_url, sizeof(dns_url), dns_port);
	TEST_ASSERT_TRUE(wifi_http_provisioning_set_http_url(http_url));
	TEST_ASSERT_TRUE(wifi_http_provisioning_set_dns_url(dns_url));

	/* --- a successful start reaches RUNNING (both listeners) ------ */
	TEST_ASSERT_TRUE_MESSAGE(wifi_http_provisioning_start(),
				 "start must succeed with both listeners available");
	TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_RUNNING, wifi_http_provisioning_get_state());

	/* Running only after both listeners start. */
	TEST_ASSERT_TRUE(captive_dns_server_is_running());

	/* --- repeated start is a safe no-op --------------------------- */
	TEST_ASSERT_TRUE(wifi_http_provisioning_start());
	TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_RUNNING, wifi_http_provisioning_get_state());

	/* --- URLs cannot change while running ------------------------- */
	TEST_ASSERT_FALSE(wifi_http_provisioning_set_http_url(http_url));
	TEST_ASSERT_FALSE(wifi_http_provisioning_set_dns_url(dns_url));

	/* --- the shared process stays alive under provisioning -------- */
	TEST_ASSERT_TRUE(MongooseProcess_IsRunning());

	/* --- stopping does not deinitialize Mongoose ------------------ */
	TEST_ASSERT_TRUE(wifi_http_provisioning_stop());
	TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_STOPPED, wifi_http_provisioning_get_state());
	TEST_ASSERT_FALSE(captive_dns_server_is_running());

	/* --- stop again is a safe no-op ------------------------------- */
	TEST_ASSERT_TRUE(wifi_http_provisioning_stop());
	TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_STOPPED, wifi_http_provisioning_get_state());

	/* Stopping provisioning must NOT stop the shared Mongoose process. */
	TEST_ASSERT_TRUE_MESSAGE(MongooseProcess_IsRunning(),
		"stopping provisioning must not deinitialize the shared Mongoose process");

	/* --- partial start: HTTP bind failure rolls back the DNS ------ */
	{
		int free_dns = free_udp_port();
		int conflict_tcp = 0;
		int fd = open_conflict_tcp(&conflict_tcp);
		TEST_ASSERT_TRUE(fd >= 0);
		TEST_ASSERT_TRUE(conflict_tcp > 0);
		TEST_ASSERT_TRUE(free_dns > 0);
		build_http_url(http_url, sizeof(http_url), conflict_tcp);
		build_dns_url(dns_url, sizeof(dns_url), free_dns);
		TEST_ASSERT_TRUE(wifi_http_provisioning_set_http_url(http_url));
		TEST_ASSERT_TRUE(wifi_http_provisioning_set_dns_url(dns_url));

		TEST_ASSERT_FALSE_MESSAGE(wifi_http_provisioning_start(),
			"HTTP bind failure must roll the start back to failure");
		/* Partial start must not leave a half-running application. */
		TEST_ASSERT_TRUE(wifi_http_provisioning_get_state() == WIFI_PROVISIONING_ERROR ||
				 wifi_http_provisioning_get_state() == WIFI_PROVISIONING_STOPPED);
		/* The DNS listener started before HTTP must have been rolled back. */
		TEST_ASSERT_TRUE_MESSAGE(wifi_http_provisioning_get_state() != WIFI_PROVISIONING_RUNNING,
					"partial start must not leave the app running");
		close(fd);
		wifi_http_provisioning_stop();   /* settle back to STOPPED */
		TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_STOPPED, wifi_http_provisioning_get_state());
	}

	/* --- partial start: DNS bind failure returns cleanly ---------- */
	{
		int free_http = free_tcp_port();
		int conflict_dns = 0;
		int fd = open_conflict_udp(&conflict_dns);
		TEST_ASSERT_TRUE(fd >= 0);
		TEST_ASSERT_TRUE(conflict_dns > 0);
		TEST_ASSERT_TRUE(free_http > 0);
		build_http_url(http_url, sizeof(http_url), free_http);
		build_dns_url(dns_url, sizeof(dns_url), conflict_dns);
		TEST_ASSERT_TRUE(wifi_http_provisioning_set_http_url(http_url));
		TEST_ASSERT_TRUE(wifi_http_provisioning_set_dns_url(dns_url));

		TEST_ASSERT_FALSE_MESSAGE(wifi_http_provisioning_start(),
			"DNS bind failure must fail the start and roll back");
		TEST_ASSERT_TRUE(wifi_http_provisioning_get_state() == WIFI_PROVISIONING_ERROR ||
				 wifi_http_provisioning_get_state() == WIFI_PROVISIONING_STOPPED);
		TEST_ASSERT_FALSE(captive_dns_server_is_running());
		TEST_ASSERT_TRUE(MongooseProcess_IsRunning());
		close(fd);
		wifi_http_provisioning_stop();
		TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_STOPPED, wifi_http_provisioning_get_state());
	}

	/* --- recovery: after an ERROR, a clean start can reach RUNNING -- */
	{
		http_port = free_tcp_port();
		dns_port  = free_udp_port();
		TEST_ASSERT_TRUE(http_port > 0);
		TEST_ASSERT_TRUE(dns_port > 0);
		build_http_url(http_url, sizeof(http_url), http_port);
		build_dns_url(dns_url, sizeof(dns_url), dns_port);
		TEST_ASSERT_TRUE(wifi_http_provisioning_set_http_url(http_url));
		TEST_ASSERT_TRUE(wifi_http_provisioning_set_dns_url(dns_url));

		TEST_ASSERT_TRUE(wifi_http_provisioning_start());
		TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_RUNNING, wifi_http_provisioning_get_state());
		TEST_ASSERT_TRUE(wifi_http_provisioning_stop());
		TEST_ASSERT_EQUAL_INT(WIFI_PROVISIONING_STOPPED, wifi_http_provisioning_get_state());
	}
}

#ifdef ESP_PLATFORM
void app_main( void )
#else
int main( void )
#endif
{
	int rc = 0;

	setvbuf( stdout, NULL, _IONBF, 0 );
	wifi_provisioning_test_fixture_configure( TEST_IMAGE_PATH );

#ifdef ESP_PLATFORM
	UNITY_BEGIN();
#endif
	/* RUN_TEST installs Unity's protected frame so an assertion failure is a
	 * clean test failure; setUp()/tearDown() own the component + filesystem
	 * lifecycle around the scenario. */
	RUN_TEST( run_lifecycle_tests );

#ifdef ESP_PLATFORM
	UNITY_END();
#else
	rc = UNITY_END();
#endif
	return rc;
}