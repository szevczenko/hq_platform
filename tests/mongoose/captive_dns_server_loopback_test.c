/*
 * Captive DNS UDP loopback test (POSIX).
 *
 * Drives the real captive DNS service over a local UDP loopback socket using a
 * non-privileged, configurable port.  The test deliberately avoids external
 * tools such as `dig` and speaks raw DNS itself:
 *  - reserve a free non-privileged port on the loopback interface,
 *  - start the service bound to udp://127.0.0.1:<port>,
 *  - send a raw single-question A query to the running service,
 *  - verify the reply carries the configured captive IPv4 address,
 *  - stop and then restart the service in the same process (same port) and
 *    repeat the query to prove the restart did not run into an address-in-use
 *    failure.
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
#error "captive_dns_server_loopback_test.c targets POSIX only"
#endif

/* 192.0.2.1 as a big-endian captive portal address. */
static const uint8_t g_ip4[4] = {192, 0, 2, 1};

void setUp(void)
{
	/* Ensure a clean slate between tests. */
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
	put16(&buf[0], 0x0eafu);   /* transaction id */
	put16(&buf[2], 0x0100u);   /* RD set, QR clear (standard query) */
	put16(&buf[4], 1);         /* QDCOUNT */
	return CAPTIVE_DNS_HEADER_LEN +
	       put_question(buf, CAPTIVE_DNS_HEADER_LEN, "example.com",
	                    1u /* A */, CAPTIVE_DNS_CLASS_IN);
}

/* Reserve a non-privileged loopback port by binding to port 0, then close it
 * so the port is free again for the DNS listener. */
static int reserve_loopback_port(void)
{
	struct sockaddr_in addr;
	socklen_t          len = sizeof(addr);
	int                fd  = (int) socket(AF_INET, SOCK_DGRAM, 0);

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

/* Open a loopback UDP client socket and send `q` to the DNS listener at
 * `port`.  Returns the number of bytes of the answer received, or 0. */
static int send_query_and_recv(struct sockaddr_in *dst, uint8_t *q, size_t qlen,
                               uint8_t *buf, size_t buf_cap)
{
	struct sockaddr_in src;
	socklen_t          src_len = sizeof(src);
	int                sock;
	int                sent;
	int                received;

	sock = (int) socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0) return 0;

	memset(&src, 0, sizeof(src));
	src.sin_family      = AF_INET;
	src.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	src.sin_port        = 0;
	if (bind(sock, (struct sockaddr *) &src, sizeof(src)) != 0) {
		close(sock);
		return 0;
	}

	set_sock_recv_timeout(sock, 2000);
	sent = (int) sendto(sock, q, (int) qlen, 0,
	                    (struct sockaddr *) dst, sizeof(*dst));
	if (sent != (int) qlen) {
		close(sock);
		return 0;
	}

	received = (int) recvfrom(sock, buf, (int) buf_cap, 0,
	                          (struct sockaddr *) &src, &src_len);
	close(sock);
	return received;
}

/* Assert that `buf[0..len)` is a valid A answer whose rdata equals g_ip4. */
static void assert_a_answer_has_captive_ip(const uint8_t *buf, size_t len,
                                           size_t qlen)
{
	size_t i;
	bool   found = false;

	TEST_ASSERT_TRUE_MESSAGE(len > 0, "must receive a DNS response");
	TEST_ASSERT_TRUE((rd16(&buf[2]) & 0x8000u) != 0u);  /* QR */
	TEST_ASSERT_EQUAL_UINT16(1u, rd16(&buf[4]));         /* QDCOUNT */
	TEST_ASSERT_EQUAL_UINT16(1u, rd16(&buf[6]));         /* ANCOUNT */
	TEST_ASSERT_TRUE(len > qlen);                        /* A answer added */

	for (i = 0; i + 4 <= len; i++) {
		if (buf[i] == g_ip4[0] && buf[i + 1] == g_ip4[1] &&
		    buf[i + 2] == g_ip4[2] && buf[i + 3] == g_ip4[3]) {
			found = true;
			break;
		}
	}
	TEST_ASSERT_TRUE_MESSAGE(found,
	                         "A answer must carry the captive IPv4 address");
}

/* ------------------------------------------------------------------ */

static void test_loopback_query_and_restart_in_same_process(void)
{
	char              url[64];
	int               port;
	uint8_t           q[512], buf[512];
	size_t            qlen;
	struct sockaddr_in dst;

	port = reserve_loopback_port();
	TEST_ASSERT_TRUE_MESSAGE(port > 0, "must reserve a non-privileged port");
	TEST_ASSERT_TRUE_MESSAGE(port >= 1024,
		"loopback test must use a non-privileged port (>= 1024)");

	build_url(url, sizeof(url), port);
	captive_dns_server_set_ip4(g_ip4);
	TEST_ASSERT_TRUE(captive_dns_server_set_bind_url(url));

	/* First run: start, query, verify. */
	TEST_ASSERT_TRUE_MESSAGE(captive_dns_server_start(),
	                         "start() must bind the listener");
	TEST_ASSERT_TRUE(captive_dns_server_is_running());

	memset(&dst, 0, sizeof(dst));
	dst.sin_family      = AF_INET;
	dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	dst.sin_port        = htons((uint16_t) port);

	qlen = build_query(q);
	int first = send_query_and_recv(&dst, q, qlen, q, sizeof(q));
	assert_a_answer_has_captive_ip(q, (size_t) first, qlen);
	TEST_ASSERT_EQUAL_UINT16(0x0eafu, rd16(&q[0]));  /* txn id preserved */

	/* Stop the service in the same process. */
	captive_dns_server_stop();
	TEST_ASSERT_FALSE(captive_dns_server_is_running());

	/* Restart on the same port in the same process and query again.  This must
	 * not fail with an address-in-use error because the previous listener was
	 * fully closed. */
	TEST_ASSERT_TRUE_MESSAGE(captive_dns_server_start(),
	                         "restart() must bind the same port again "
	                         "(no address-in-use)");
	TEST_ASSERT_TRUE(captive_dns_server_is_running());

	int second = send_query_and_recv(&dst, q, qlen, q, sizeof(q));
	assert_a_answer_has_captive_ip(q, (size_t) second, qlen);

	captive_dns_server_stop();
}

/* ------------------------------------------------------------------ */

static void captive_dns_server_loopback_tests_run(void)
{
	RUN_TEST(test_loopback_query_and_restart_in_same_process);
}

#ifdef ESP_PLATFORM
void app_main(void)
{
	UNITY_BEGIN();
	captive_dns_server_loopback_tests_run();
	UNITY_END();
}
#else
int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	UNITY_BEGIN();
	captive_dns_server_loopback_tests_run();
	return UNITY_END();
}
#endif