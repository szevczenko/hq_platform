/*
 * Captive DNS UDP service lifecycle unit tests (POSIX).
 *
 * Exercises the reusable Mongoose UDP listener wrapper:
 *  - instant start/stop are idempotent,
 *  - starting reports a bind failure when the UDP port is already taken,
 *  - a real DNS datagram is answered with the configured captive A record,
 *  - stopping the DNS service does not stop the shared Mongoose process,
 *  - a later start after stop binds a fresh listener on the same service.
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "captive_dns_server.h"
#include "mongoose_process.h"
#include "unity.h"

#ifdef ESP_PLATFORM
#error "captive_dns_server_lifecycle_test.c targets POSIX only"
#endif

/* 192.0.2.1 as a big-endian captive portal address. */
static const uint8_t g_ip4[4] = {192, 0, 2, 1};

void setUp(void)
{
	/* Ensure a clean slate between tests (process may still be running from
	 * a previous test's tearDown ordering). */
	captive_dns_server_stop();
	MongooseProcess_Deinit();
}

void tearDown(void)
{
	captive_dns_server_stop();  /* Idempotent; safe even when not running. */
	MongooseProcess_Deinit();
}

static uint16_t rd16(const uint8_t *p)
{
	return (uint16_t)(((uint16_t) p[0] << 8) | p[1]);
}

static void put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t) (v >> 8);
	p[1] = (uint8_t) (v & 0xFFu);
}

/* Encode one DNS question (label name + type/class) at buf[ofs..].  Returns
 * the number of bytes written. */
static size_t put_question(uint8_t *buf, size_t ofs, const char *name,
                           uint16_t qtype, uint16_t qclass)
{
	size_t p = ofs, start = 0;
	while (name[start] != '\0') {
		size_t end = start;
		while (name[end] != '\0' && name[end] != '.') end++;
		buf[p++] = (uint8_t) (end - start);
		for (size_t k = start; k < end; k++) buf[p++] = (uint8_t) name[k];
		if (name[end] == '\0') break;
		start = end + 1;
	}
	buf[p++] = 0;   /* root label */
	put16(&buf[p], qtype);
	p += 2;
	put16(&buf[p], qclass);
	p += 2;
	return p - ofs;
}

/* Build a single-question standard A query; returns the total length. */
static size_t build_query(uint8_t *buf)
{
	uint16_t i;
	for (i = 0; i < CAPTIVE_DNS_HEADER_LEN; i++) buf[i] = 0;
	put16(&buf[0], 0xBEAAu);   /* transaction id */
	put16(&buf[2], 0x0100u);   /* RD set, QR clear (standard query) */
	put16(&buf[4], 1);         /* QDCOUNT */
	return CAPTIVE_DNS_HEADER_LEN +
	       put_question(buf, CAPTIVE_DNS_HEADER_LEN, "example.com",
	                    1u /* A */, CAPTIVE_DNS_CLASS_IN);
}

/* Open a UDP socket bound to 127.0.0.1 on an OS-assigned port and return the
 * port number.  The socket is NOT set SO_REUSEADDR, so a second bind to the
 * same port (as mongoose does) must fail. */
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
	if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) != 0) {
		close(fd);
		return -2;
	}
	if (getsockname(fd, (struct sockaddr *) &addr, &len) != 0) {
		close(fd);
		return -3;
	}
	*port = ntohs(addr.sin_port);
	return fd;
}

/* Reserve a port by binding to port 0, then close it so the port is free. */
static int free_port(void)
{
	struct sockaddr_in addr;
	socklen_t          len = sizeof(addr);
	int                fd = (int) socket(AF_INET, SOCK_DGRAM, 0);

	if (fd < 0) return -1;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family      = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port        = 0;
	if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) != 0) {
		close(fd);
		return -1;
	}
	if (getsockname(fd, (struct sockaddr *) &addr, &len) != 0) {
		close(fd);
		return -1;
	}
	int port = ntohs(addr.sin_port);
	close(fd);
	return port;
}

static void build_url(char *out, size_t out_cap, int port)
{
	snprintf(out, out_cap, "udp://127.0.0.1:%d", port);
}

static void set_sock_recv_timeout(int fd, int ms)
{
	struct timeval tv;
	tv.tv_sec  = ms / 1000;
	tv.tv_usec = (ms % 1000) * 1000;
	(void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

/* ------------------------------------------------------------------ */

static void test_start_stop_are_idempotent_and_process_stays_alive(void)
{
	char url[64];
	int  port = free_port();

	TEST_ASSERT_TRUE(port > 0);
	build_url(url, sizeof(url), port);
	TEST_ASSERT_TRUE(captive_dns_server_set_bind_url(url));
	captive_dns_server_set_ip4(g_ip4);

	/* Start binds the listener; a second start is a safe no-op. */
	TEST_ASSERT_TRUE_MESSAGE(captive_dns_server_start(),
				 "start() must bind the listener");
	TEST_ASSERT_TRUE(captive_dns_server_is_running());
	TEST_ASSERT_TRUE_MESSAGE(captive_dns_server_start(),
				 "second start() must be a safe no-op");
	TEST_ASSERT_TRUE(captive_dns_server_is_running());

	/* The shared Mongoose process keeps running under DNS. */
	TEST_ASSERT_TRUE_MESSAGE(MongooseProcess_IsRunning(),
				 "DNS start() must not tear down the process");

	/* Stop releases the listener; a second stop is also safe. */
	captive_dns_server_stop();
	TEST_ASSERT_FALSE(captive_dns_server_is_running());
	captive_dns_server_stop();  /* must not crash */

	/* Stopping DNS does NOT stop the shared Mongoose process. */
	TEST_ASSERT_TRUE_MESSAGE(MongooseProcess_IsRunning(),
				 "stopping DNS must not stop the shared process");
}

static void test_bind_failure_is_reported(void)
{
	char url[64];
	int  port = 0;

	int fd = open_conflict_udp(&port);
	TEST_ASSERT_TRUE(fd >= 0);
	TEST_ASSERT_TRUE(port > 0);
	build_url(url, sizeof(url), port);
	TEST_ASSERT_TRUE(captive_dns_server_set_bind_url(url));

	/* The port is held by another socket, so the DNS bind must fail and the
	 * server must report it to the caller. */
	TEST_ASSERT_FALSE_MESSAGE(captive_dns_server_start(),
				  "start() must report a bind failure");
	TEST_ASSERT_FALSE(captive_dns_server_is_running());

	/* After the conflicting socket is closed, the same service can start. */
	close(fd);
	captive_dns_server_start();
	TEST_ASSERT_TRUE(captive_dns_server_is_running());
	captive_dns_server_stop();
}

static void test_answers_dns_query(void)
{
	char url[64];
	int  port;
	int  sock;
	int  reply_len;
	struct sockaddr_in dst, src;
	uint8_t q[512], buf[512];
	socklen_t  src_len = sizeof(src);

	port = free_port();
	TEST_ASSERT_TRUE(port > 0);
	build_url(url, sizeof(url), port);

	captive_dns_server_set_ip4(g_ip4);
	TEST_ASSERT_TRUE(captive_dns_server_set_bind_url(url));
	TEST_ASSERT_TRUE(captive_dns_server_start());

	size_t qlen = build_query(q);

	/* Client socket used to send the query and receive the reply. */
	sock = (int) socket(AF_INET, SOCK_DGRAM, 0);
	TEST_ASSERT_TRUE(sock >= 0);
	memset(&src, 0, sizeof(src));
	src.sin_family      = AF_INET;
	src.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	src.sin_port        = 0;
	TEST_ASSERT_EQUAL_INT(0, bind(sock, (struct sockaddr *) &src,
				      sizeof(src)));

	memset(&dst, 0, sizeof(dst));
	dst.sin_family      = AF_INET;
	dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	dst.sin_port        = htons((uint16_t)port);

	set_sock_recv_timeout(sock, 2000);
	TEST_ASSERT_EQUAL_INT((int)qlen, sendto(sock, q, (int)qlen, 0,
			      (struct sockaddr *) &dst, sizeof(dst)));

	reply_len = (int)recvfrom(sock, buf, sizeof(buf), 0,
				  (struct sockaddr *) &src, &src_len);
	TEST_ASSERT_TRUE_MESSAGE(reply_len > 0,
				"must receive a DNS response to the query");

	/* The reply must preserve the transaction id and be an authoritative A
	 * answer carrying the configured captive address. */
	TEST_ASSERT_EQUAL_UINT16(0xBEAAu, rd16(&buf[0]));
	TEST_ASSERT_TRUE((rd16(&buf[2]) & 0x8000u) != 0u);   /* QR */
	TEST_ASSERT_EQUAL_UINT16(1u, rd16(&buf[4]));         /* QDCOUNT */
	TEST_ASSERT_EQUAL_UINT16(1u, rd16(&buf[6]));         /* ANCOUNT */
	TEST_ASSERT_TRUE((size_t)reply_len > qlen);          /* A answer added */

	/* The A answer IPv4 rdata must equal the configured captive address. */
	{
		size_t i;
		bool   found = false;
		for (i = 0; i + 4 <= (size_t)reply_len; i++) {
			if (buf[i] == g_ip4[0] && buf[i + 1] == g_ip4[1] &&
			    buf[i + 2] == g_ip4[2] && buf[i + 3] == g_ip4[3]) {
				found = true;
				break;
			}
		}
		TEST_ASSERT_TRUE_MESSAGE(found,
					 "A answer must carry the captive IPv4");
	}

	close(sock);
	captive_dns_server_stop();
}

/* ------------------------------------------------------------------ */

static void captive_dns_server_lifecycle_tests_run(void)
{
	RUN_TEST(test_start_stop_are_idempotent_and_process_stays_alive);
	RUN_TEST(test_bind_failure_is_reported);
	RUN_TEST(test_answers_dns_query);
}

#ifdef ESP_PLATFORM
void app_main(void)
{
	UNITY_BEGIN();
	captive_dns_server_lifecycle_tests_run();
	UNITY_END();
}
#else
int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	UNITY_BEGIN();
	captive_dns_server_lifecycle_tests_run();
	return UNITY_END();
}
#endif