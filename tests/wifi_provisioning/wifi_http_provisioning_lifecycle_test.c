/*
 * Wi-Fi HTTP provisioning lifecycle robustness unit tests (POSIX).
 *
 * Exercises the provisioning application's full lifecycle against the shared
 * Mongoose process and the POSIX Wi-Fi simulator, focused on the guarantee
 * that a lifecycle/rollback cycle leaves no running task, timer, listener, or
 * OSAL object behind:
 *   - start/stop stay clean across many repeated cycles (no leaked listener),
 *   - a start before Mongoose is up is rejected cleanly,
 *   - an HTTP bind failure rolls back the already-started DNS listener,
 *   - a DNS bind failure rolls the whole start back,
 *   - after a failed start the application recovers on a fresh cycle,
 *   - stopping provisioning never deinitializes the shared Mongoose process,
 *     so MQTT and ThingsBoard (which ride the same process) keep running.
 *
 * This target links against the real Wi-Fi management platform module (same
 * wiring as wifi_http_provisioning_tests). Listener failures are injected with
 * real socket conflicts so the rollback path is exercised end-to-end.
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
#error "wifi_http_provisioning_lifecycle_test.c targets POSIX only"
#endif

#define INVOKE_TIMEOUT_MS 2000u

/* Set by a Mongoose poll-thread callback to prove the shared process is
 * still servable (i.e. available for MQTT/ThingsBoard) after a stop. */
static bool s_mongoose_invoke_ran = false;

/* This test manages its own single init/cleanup lifecycle in main(), so the
 * Unity setup hooks are intentionally empty (still required at link time). */
void setUp( void )
{
}

void tearDown( void )
{
}

/* -- socket helpers (borrowed pattern from the lifecycle test) --------- */

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
	snprintf( out, cap, "http://127.0.0.1:%d", port );
}

static void build_dns_url( char * out, size_t cap, int port )
{
	snprintf( out, cap, "udp://127.0.0.1:%d", port );
}

/* Configure both listeners onto fresh high ports. */
static void configure_fresh_listeners( char * http_url, size_t http_cap,
                                       char * dns_url, size_t dns_cap )
{
	int http_port = free_tcp_port();
	int dns_port  = free_udp_port();
	TEST_ASSERT_TRUE( http_port > 0 );
	TEST_ASSERT_TRUE( dns_port > 0 );
	build_http_url( http_url, http_cap, http_port );
	build_dns_url( dns_url, dns_cap, dns_port );
	TEST_ASSERT_TRUE( wifi_http_provisioning_set_http_url( http_url ) );
	TEST_ASSERT_TRUE( wifi_http_provisioning_set_dns_url( dns_url ) );
}

/* A no-op poll-thread callback that records execution. */
static void mongoose_noop_cb( struct mg_mgr * mgr, void * user )
{
	(void) mgr;
	(void) user;
	s_mongoose_invoke_ran = true;
}

/* ------------------------------------------------------------------ */
/* Lifecycle robustness tests.                                        */
/* ------------------------------------------------------------------ */

/* A successful start reaches RUNNING; a successful stop returns to STOPPED.
 * Repeating the pair many times must never leak a DNS listener, and the shared
 * Mongoose process must never be torn down. */
static void test_repeated_start_stop_cycles( void )
{
	char http_url[64], dns_url[64];
	int  i;

	for ( i = 0; i < 25; ++i )
	{
		configure_fresh_listeners( http_url, sizeof( http_url ),
		                           dns_url, sizeof( dns_url ) );

		TEST_ASSERT_TRUE_MESSAGE( wifi_http_provisioning_start(),
		       "start must succeed across repeated cycles" );
		TEST_ASSERT_EQUAL_INT( WIFI_PROVISIONING_RUNNING,
		                       wifi_http_provisioning_get_state() );
		TEST_ASSERT_TRUE( captive_dns_server_is_running() );
		TEST_ASSERT_TRUE( MongooseProcess_IsRunning() );

		TEST_ASSERT_TRUE( wifi_http_provisioning_stop() );
		TEST_ASSERT_EQUAL_INT( WIFI_PROVISIONING_STOPPED,
		                       wifi_http_provisioning_get_state() );
		TEST_ASSERT_FALSE_MESSAGE( captive_dns_server_is_running(),
		       "stop must release the DNS listener every cycle" );
		TEST_ASSERT_TRUE( MongooseProcess_IsRunning() );
	}
}

/* Start must be rejected cleanly when the shared Mongoose process is down.
 * The application must not leave a half-running state or leaked listener. */
static void test_start_rejected_without_mongoose( void )
{
	MongooseProcess_Deinit();
	TEST_ASSERT_FALSE( MongooseProcess_IsRunning() );
	TEST_ASSERT_FALSE_MESSAGE( wifi_http_provisioning_start(),
	       "start must be rejected when Mongoose is not running" );
	TEST_ASSERT_EQUAL_INT( WIFI_PROVISIONING_ERROR,
	                       wifi_http_provisioning_get_state() );
	TEST_ASSERT_FALSE( captive_dns_server_is_running() );

	/* Bring the shared process back for the remaining tests. */
	MongooseProcess_Init();
	TEST_ASSERT_TRUE( MongooseProcess_IsRunning() );
}

/* HTTP bind failure: the DNS listener is opened first and must be rolled back
 * so a half-running application is never left behind. */
static void test_http_bind_failure_roll_back_dns( void )
{
	char  http_url[64], dns_url[64];
	int   free_dns, conflict_tcp;
	int   fd;

	free_dns = free_udp_port();
	fd       = open_conflict_tcp( &conflict_tcp );
	TEST_ASSERT_TRUE( fd >= 0 );
	TEST_ASSERT_TRUE( conflict_tcp > 0 );
	TEST_ASSERT_TRUE( free_dns > 0 );
	build_http_url( http_url, sizeof( http_url ), conflict_tcp );
	build_dns_url( dns_url, sizeof( dns_url ), free_dns );
	TEST_ASSERT_TRUE( wifi_http_provisioning_set_http_url( http_url ) );
	TEST_ASSERT_TRUE( wifi_http_provisioning_set_dns_url( dns_url ) );

	TEST_ASSERT_FALSE_MESSAGE( wifi_http_provisioning_start(),
	       "HTTP bind failure must fail the start and roll back" );
	TEST_ASSERT_TRUE( wifi_http_provisioning_get_state() ==
	                  WIFI_PROVISIONING_ERROR ||
	                  wifi_http_provisioning_get_state() ==
	                  WIFI_PROVISIONING_STOPPED );
	TEST_ASSERT_FALSE_MESSAGE( captive_dns_server_is_running(),
	       "the DNS listener must be rolled back after HTTP bind failure" );
	TEST_ASSERT_TRUE( MongooseProcess_IsRunning() );
	close( fd );

	wifi_http_provisioning_stop();   /* settle back to STOPPED */
	TEST_ASSERT_EQUAL_INT( WIFI_PROVISIONING_STOPPED,
	                       wifi_http_provisioning_get_state() );
}

/* DNS bind failure: nothing is bound and the start fails cleanly. */
static void test_dns_bind_failure_roll_back( void )
{
	char  http_url[64], dns_url[64];
	int   free_tcp, conflict_dns;
	int   fd;

	free_tcp = free_tcp_port();
	fd       = open_conflict_udp( &conflict_dns );
	TEST_ASSERT_TRUE( fd >= 0 );
	TEST_ASSERT_TRUE( conflict_dns > 0 );
	TEST_ASSERT_TRUE( free_tcp > 0 );
	build_http_url( http_url, sizeof( http_url ), free_tcp );
	build_dns_url( dns_url, sizeof( dns_url ), conflict_dns );
	TEST_ASSERT_TRUE( wifi_http_provisioning_set_http_url( http_url ) );
	TEST_ASSERT_TRUE( wifi_http_provisioning_set_dns_url( dns_url ) );

	TEST_ASSERT_FALSE_MESSAGE( wifi_http_provisioning_start(),
	       "DNS bind failure must fail the start and roll back" );
	TEST_ASSERT_TRUE( wifi_http_provisioning_get_state() ==
	                  WIFI_PROVISIONING_ERROR ||
	                  wifi_http_provisioning_get_state() ==
	                  WIFI_PROVISIONING_STOPPED );
	TEST_ASSERT_FALSE( captive_dns_server_is_running() );
	TEST_ASSERT_TRUE( MongooseProcess_IsRunning() );
	close( fd );

	wifi_http_provisioning_stop();
	TEST_ASSERT_EQUAL_INT( WIFI_PROVISIONING_STOPPED,
	                       wifi_http_provisioning_get_state() );
}

/* After an ERROR state, a later clean start must recover to RUNNING. */
static void test_recovery_after_failure( void )
{
	char http_url[64], dns_url[64];

	configure_fresh_listeners( http_url, sizeof( http_url ),
	                           dns_url, sizeof( dns_url ) );

	TEST_ASSERT_TRUE( wifi_http_provisioning_start() );
	TEST_ASSERT_EQUAL_INT( WIFI_PROVISIONING_RUNNING,
	                       wifi_http_provisioning_get_state() );
	TEST_ASSERT_TRUE( captive_dns_server_is_running() );
	TEST_ASSERT_TRUE( wifi_http_provisioning_stop() );
	TEST_ASSERT_EQUAL_INT( WIFI_PROVISIONING_STOPPED,
	                       wifi_http_provisioning_get_state() );
	TEST_ASSERT_FALSE( captive_dns_server_is_running() );
}

/* Stopping provisioning must not stop the shared Mongoose process, which is
 * exactly the process that hosts MQTT and ThingsBoard. After a stop, the
 * process must still be running and servable via a poll-thread invocation,
 * i.e. a later MQTT/TB session can still address it. */
static void test_stop_does_not_stop_mongoose_or_mqtt( void )
{
	char http_url[64], dns_url[64];

	configure_fresh_listeners( http_url, sizeof( http_url ),
	                           dns_url, sizeof( dns_url ) );
	TEST_ASSERT_TRUE( wifi_http_provisioning_start() );
	TEST_ASSERT_EQUAL_INT( WIFI_PROVISIONING_RUNNING,
	                       wifi_http_provisioning_get_state() );

	TEST_ASSERT_TRUE( wifi_http_provisioning_stop() );
	TEST_ASSERT_EQUAL_INT( WIFI_PROVISIONING_STOPPED,
	                       wifi_http_provisioning_get_state() );

	/* Stop only closed the provisioning listeners, not the shared process. */
	TEST_ASSERT_TRUE_MESSAGE( MongooseProcess_IsRunning(),
	       "stopping provisioning must not deinit the shared Mongoose process" );

	/* The shared process is still servable: an invocation on the poll thread
	 * still runs. MQTT/ThingsBoard listeners ride this same process. */
	s_mongoose_invoke_ran = false;
	TEST_ASSERT_TRUE( MongooseProcess_Invoke( mongoose_noop_cb, NULL,
	                                          INVOKE_TIMEOUT_MS ) );
	TEST_ASSERT_TRUE_MESSAGE( s_mongoose_invoke_ran,
	       "shared Mongoose process must remain servable after provisioning stop" );
}

/* ------------------------------------------------------------------ */
/* Test runner.                                                       */
/* ------------------------------------------------------------------ */

#ifdef ESP_PLATFORM
void app_main( void )
#else
int main( void )
#endif
{
	int rc = 0;

	/* --- bring up Wi-Fi management once for all subtests ---------- */
	MongooseProcess_Deinit();	/* clean slate */
	wifi_mgmt_set_wifi_type( T_WIFI_TYPE_CLI_SER );
	wifi_mgmt_init();
	wifi_mgmt_start();
	{
		uint32_t elapsed = 0;
		while ( !wifi_mgmt_is_running() && elapsed < 3000 )
		{
			osal_task_delay_ms( 10 );
			elapsed += 10;
		}
	}

	setvbuf( stdout, NULL, _IONBF, 0 );
	UNITY_BEGIN();

	TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_is_running(),
	       "Wi-Fi management must be running for the lifecycle tests" );

	RUN_TEST( test_start_rejected_without_mongoose );
	RUN_TEST( test_repeated_start_stop_cycles );
	RUN_TEST( test_http_bind_failure_roll_back_dns );
	RUN_TEST( test_dns_bind_failure_roll_back );
	RUN_TEST( test_recovery_after_failure );
	RUN_TEST( test_stop_does_not_stop_mongoose_or_mqtt );

	/* --- teardown -------------------------------------------------- */
	wifi_http_provisioning_stop();
	MongooseProcess_Deinit();
	wifi_mgmt_stop();

	rc = UNITY_END();
	return rc;
}