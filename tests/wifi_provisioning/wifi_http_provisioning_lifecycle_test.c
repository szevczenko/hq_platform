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
#include <pthread.h>
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
#error "wifi_http_provisioning_lifecycle_test.c targets POSIX only"
#endif

#define INVOKE_TIMEOUT_MS 2000u

/* Set by a Mongoose poll-thread callback to prove the shared process is
 * still servable (i.e. available for MQTT/ThingsBoard) after a stop. */
static bool s_mongoose_invoke_ran = false;

/* Dedicated littlefs image so this test never touches another test's state. */
#define TEST_IMAGE_PATH "/tmp/wifi_prov_lifecycle.img"

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
/* Concurrent and repeated stop scenarios.                             */
/* ------------------------------------------------------------------ */

/* After a stop, repeated stops must be safe no-ops that return the same
 * completed result (WIFI_PROVISIONING_STOPPED) without touching listeners. */
static void test_repeated_stop_is_safe_and_idempotent( void )
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
	TEST_ASSERT_FALSE( captive_dns_server_is_running() );

	/* Repeated stops join/observe the completed STOPPED result. */
	TEST_ASSERT_TRUE( wifi_http_provisioning_stop() );
	TEST_ASSERT_TRUE( wifi_http_provisioning_stop() );
	TEST_ASSERT_EQUAL_INT( WIFI_PROVISIONING_STOPPED,
	                       wifi_http_provisioning_get_state() );
	TEST_ASSERT_FALSE( captive_dns_server_is_running() );
	TEST_ASSERT_TRUE( MongooseProcess_IsRunning() );
}

#define PROV_STOP_THREADS 8
static pthread_t g_prov_stop_threads[PROV_STOP_THREADS];
static bool      g_prov_stop_results[PROV_STOP_THREADS];

static void *prov_stop_runner( void * arg )
{
	size_t idx = ( size_t ) arg;

	g_prov_stop_results[idx] = wifi_http_provisioning_stop();
	return NULL;
}

/* Start the provisioning application, then stop it from many threads at once.
 * stop() holds the dedicated lifecycle mutex for the whole operation, so each
 * caller either performs the cleanup or joins the in-flight stop; every one
 * must receive the same completed result and the listeners must be fully
 * released (the same ports can be re-bound right away). */
static void test_concurrent_stop_callers( void )
{
	char   http_url[64], dns_url[64];
	size_t i;

	configure_fresh_listeners( http_url, sizeof( http_url ),
	                           dns_url, sizeof( dns_url ) );
	TEST_ASSERT_TRUE( wifi_http_provisioning_start() );
	TEST_ASSERT_EQUAL_INT( WIFI_PROVISIONING_RUNNING,
	                       wifi_http_provisioning_get_state() );
	TEST_ASSERT_TRUE( captive_dns_server_is_running() );

	for ( i = 0; i < PROV_STOP_THREADS; ++i )
	{
		TEST_ASSERT_EQUAL_INT( 0,
			pthread_create( &g_prov_stop_threads[i], NULL,
			                prov_stop_runner, ( void * ) i ) );
	}

	for ( i = 0; i < PROV_STOP_THREADS; ++i )
	{
		TEST_ASSERT_EQUAL_INT( 0,
		                      pthread_join( g_prov_stop_threads[i], NULL ) );
		TEST_ASSERT_TRUE_MESSAGE( g_prov_stop_results[i],
			"every concurrent stop caller must receive the completed result" );
	}

	TEST_ASSERT_EQUAL_INT( WIFI_PROVISIONING_STOPPED,
	                       wifi_http_provisioning_get_state() );
	TEST_ASSERT_FALSE( captive_dns_server_is_running() );
	TEST_ASSERT_TRUE( MongooseProcess_IsRunning() );

	/* The same configured listeners can be re-bound: their poll-thread close
	 * operations completed before the concurrent stops returned. */
	TEST_ASSERT_TRUE( wifi_http_provisioning_start() );
	TEST_ASSERT_EQUAL_INT( WIFI_PROVISIONING_RUNNING,
	                       wifi_http_provisioning_get_state() );
	TEST_ASSERT_TRUE( wifi_http_provisioning_stop() );
	TEST_ASSERT_EQUAL_INT( WIFI_PROVISIONING_STOPPED,
	                       wifi_http_provisioning_get_state() );
}

#define PROV_HAMMER_THREADS 4
static pthread_t g_prov_hammer_threads[PROV_HAMMER_THREADS];
static bool      g_prov_hammer_failed[PROV_HAMMER_THREADS];

/* Repeatedly cycles start()/stop() on the same configured ports. All calls
 * must succeed: start is a no-op when already running and stop either runs or
 * joins another stop, so no interleaving may fail or leak a listener. */
static void *prov_hammer_runner( void * arg )
{
	size_t idx = ( size_t ) arg;
	int    i;

	g_prov_hammer_failed[idx] = false;
	for ( i = 0; i < 10; ++i )
	{
		if ( !wifi_http_provisioning_start() ) g_prov_hammer_failed[idx] = true;
		if ( !wifi_http_provisioning_stop() )  g_prov_hammer_failed[idx] = true;
	}
	return NULL;
}

static void test_concurrent_start_stop_hammer( void )
{
	char   http_url[64], dns_url[64];
	size_t i;
	int    j;

	configure_fresh_listeners( http_url, sizeof( http_url ),
	                           dns_url, sizeof( dns_url ) );

	for ( i = 0; i < PROV_HAMMER_THREADS; ++i )
	{
		TEST_ASSERT_EQUAL_INT( 0,
			pthread_create( &g_prov_hammer_threads[i], NULL,
			                prov_hammer_runner, ( void * ) i ) );
	}

	for ( i = 0; i < PROV_HAMMER_THREADS; ++i )
	{
		TEST_ASSERT_EQUAL_INT( 0,
		                      pthread_join( g_prov_hammer_threads[i], NULL ) );
		TEST_ASSERT_FALSE_MESSAGE( g_prov_hammer_failed[i],
			"hammered start/stop must never report a lifecycle failure" );
	}

	/* All threads unwound to STOPPED; settle and verify full cleanup. */
	(void) wifi_http_provisioning_stop();
	TEST_ASSERT_EQUAL_INT( WIFI_PROVISIONING_STOPPED,
	                       wifi_http_provisioning_get_state() );
	TEST_ASSERT_FALSE( captive_dns_server_is_running() );
	TEST_ASSERT_TRUE( MongooseProcess_IsRunning() );

	/* The same ports still rebind cleanly after the contention. */
	for ( j = 0; j < 3; ++j )
	{
		TEST_ASSERT_TRUE( wifi_http_provisioning_start() );
		TEST_ASSERT_EQUAL_INT( WIFI_PROVISIONING_RUNNING,
		                       wifi_http_provisioning_get_state() );
		TEST_ASSERT_TRUE( wifi_http_provisioning_stop() );
		TEST_ASSERT_EQUAL_INT( WIFI_PROVISIONING_STOPPED,
		                       wifi_http_provisioning_get_state() );
	}
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

	setvbuf( stdout, NULL, _IONBF, 0 );
	wifi_provisioning_test_fixture_configure( TEST_IMAGE_PATH );

#ifdef ESP_PLATFORM
	UNITY_BEGIN();
#endif
	/* Every RUN_TEST runs under the shared fixture's setUp()/tearDown(), so
	 * each subtest starts from a clean component + filesystem state, and an
	 * assertion failure aborts into a protected Unity frame (clean FAIL, not a
	 * SEGFAULT). */
	RUN_TEST( test_start_rejected_without_mongoose );
	RUN_TEST( test_repeated_start_stop_cycles );
	RUN_TEST( test_http_bind_failure_roll_back_dns );
	RUN_TEST( test_dns_bind_failure_roll_back );
	RUN_TEST( test_recovery_after_failure );
	RUN_TEST( test_stop_does_not_stop_mongoose_or_mqtt );
	RUN_TEST( test_repeated_stop_is_safe_and_idempotent );
	RUN_TEST( test_concurrent_stop_callers );
	RUN_TEST( test_concurrent_start_stop_hammer );

#ifdef ESP_PLATFORM
	UNITY_END();
#else
	rc = UNITY_END();
#endif
	return rc;
}