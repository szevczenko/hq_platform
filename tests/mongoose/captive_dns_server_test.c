/*
 * Captive DNS server packet codec unit tests.
 *
 * Exercises captive_dns_parse_query() and captive_dns_build_response():
 *  - bounded rejection of truncated/oversized/multi-question/zero-question,
 *    unsupported-opcode and non-query packets,
 *  - authoritative A responses that preserve the transaction ID and question,
 *  - valid answer-free responses for unsupported record types,
 *  - bounded output behaviour (small destination buffers are refused).
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "captive_dns_server.h"
#include "unity.h"

/* Record types used by the test (numbers match mg_dns.h in mongoose.h). */
#define TEST_QTYPE_A    1u
#define TEST_QTYPE_AAAA 28u

/* Fixed size of one A answer RR: 2 (name pointer) + 2 (type) + 2 (class) +
 * 4 (ttl) + 2 (rdlength) + 4 (rdata). */
#define TEST_ANSWER_LEN 16u

/* 192.0.2.1 as a big-endian 4-byte captive address. */
static const uint8_t g_ip4[4] = {192, 0, 2, 1};

void setUp(void)
{
}

void tearDown(void)
{
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

/* Write a standard-query DNS header with the given question count. */
static void put_header(uint8_t *buf, uint16_t txn, uint16_t flags, uint16_t qd)
{
	uint16_t i;
	for (i = 0; i < CAPTIVE_DNS_HEADER_LEN; i++) buf[i] = 0;
	put16(&buf[0], txn);
	put16(&buf[2], flags);
	put16(&buf[4], qd);
}

/* Encode one DNS question (label name + type/class) at buf[ofs..].
 * Returns the number of bytes written. */
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

/* Build a single-question query; returns the total packet length. */
static size_t build_query(uint8_t *buf, uint16_t qtype, uint16_t qclass)
{
	put_header(buf, 0xBEEFu, 0x0100u, 1);   /* RD set, QR clear */
	return CAPTIVE_DNS_HEADER_LEN +
	       put_question(buf, CAPTIVE_DNS_HEADER_LEN, "example.com",
	                    qtype, qclass);
}

/* Build a single-question query whose QNAME is a 2-byte name-compression
 * pointer (RFC 1035 section 4.1.4) into the length-prefixed name that
 * follows the type/class in the same packet.  This is a valid question form
 * that the bounded Mongoose parser resolves through the pointer. */
static size_t build_compressed_query(uint8_t *buf, uint16_t qtype)
{
	put_header(buf, 0x5150u, 0x0100u, 1);   /* RD set, QR clear */
	buf[12] = 0xC0u;                 /* compression pointer tag */
	buf[13] = 0x12u;                 /* ...to offset 0x0012 (name below) */
	put16(&buf[14], qtype);
	put16(&buf[16], CAPTIVE_DNS_CLASS_IN);
	buf[18] = 0x07u;                 /* label "example" */
	memcpy(&buf[19], "example", 7);
	buf[26] = 0x03u;                 /* label "com" */
	memcpy(&buf[27], "com", 3);
	buf[30] = 0x00u;                 /* root label */
	return 31u;
}

/* ------------------------------------------------------------------ */

static void test_truncated_packet_is_rejected(void)
{
	uint8_t buf[15], out[128];
	/* Every packet shorter than the header must be rejected. */
	for (size_t len = 0; len < CAPTIVE_DNS_HEADER_LEN; len++) {
		for (size_t i = 0; i < len; i++) buf[i] = (uint8_t) (i * 7u);
		captive_dns_query_t q = captive_dns_parse_query(buf, len);
		TEST_ASSERT_EQUAL_MESSAGE(CAPTIVE_DNS_KIND_NONE, q.kind,
					  "truncated packet must be rejected");
		TEST_ASSERT_EQUAL_UINT32_MESSAGE(
			0u, captive_dns_build_response(buf, len, g_ip4, out, sizeof(out)),
			"truncated packet must not generate a response");
	}
}

static void test_oversized_packet_is_rejected(void)
{
	size_t   len = CAPTIVE_DNS_MAX_PACKET + 1;
	uint8_t *buf = malloc(len);
	uint8_t  out[512];
	TEST_ASSERT_NOT_NULL(buf);
	memset(buf, 0, len);
	/* Plausible header with one question; only the size is the problem. */
	put16(&buf[0], 0x1234u);
	put16(&buf[4], 1);
	captive_dns_query_t q = captive_dns_parse_query(buf, len);
	TEST_ASSERT_EQUAL(CAPTIVE_DNS_KIND_NONE, q.kind);
	TEST_ASSERT_EQUAL_UINT32_MESSAGE(
		0u, captive_dns_build_response(buf, len, g_ip4, out, sizeof(out)),
		"oversized packet must not generate a response");
	free(buf);
}

static void test_zero_questions_is_rejected(void)
{
	uint8_t buf[64], out[128];
	put_header(buf, 0x1111u, 0x0100u, 0);   /* QDCOUNT = 0 */
	captive_dns_query_t q = captive_dns_parse_query(buf, sizeof(buf));
	TEST_ASSERT_EQUAL(CAPTIVE_DNS_KIND_NONE, q.kind);
	TEST_ASSERT_EQUAL_UINT32_MESSAGE(
		0u, captive_dns_build_response(buf, sizeof(buf), g_ip4, out, sizeof(out)),
		"zero-question packet must not generate a response");
}

static void test_multi_question_packet_is_rejected(void)
{
	uint8_t buf[256], out[256];
	size_t  p;
	put_header(buf, 0xABCDu, 0x0100u, 2);   /* QDCOUNT = 2 */
	p = CAPTIVE_DNS_HEADER_LEN;
	p += put_question(buf, p, "example.com", TEST_QTYPE_A, CAPTIVE_DNS_CLASS_IN);
	p += put_question(buf, p, "example.org", TEST_QTYPE_A, CAPTIVE_DNS_CLASS_IN);
	captive_dns_query_t q = captive_dns_parse_query(buf, p);
	TEST_ASSERT_EQUAL(CAPTIVE_DNS_KIND_NONE, q.kind);
	TEST_ASSERT_EQUAL_UINT32_MESSAGE(
		0u, captive_dns_build_response(buf, p, g_ip4, out, sizeof(out)),
		"multi-question packet must not generate a response");
}

static void test_unsupported_opcode_is_rejected(void)
{
	uint8_t buf[64], out[128];
	size_t  p;
	/* Opcode 1 (inverse query) in the header flags. */
	put_header(buf, 0x0001u, 0x0800u, 1);
	p = CAPTIVE_DNS_HEADER_LEN;
	p += put_question(buf, p, "example.com", TEST_QTYPE_A, CAPTIVE_DNS_CLASS_IN);
	captive_dns_query_t q = captive_dns_parse_query(buf, p);
	TEST_ASSERT_EQUAL(CAPTIVE_DNS_KIND_NONE, q.kind);
	TEST_ASSERT_EQUAL_UINT32_MESSAGE(
		0u, captive_dns_build_response(buf, p, g_ip4, out, sizeof(out)),
		"unsupported-opcode packet must not generate a response");
}

static void test_response_packet_is_ignored(void)
{
	uint8_t buf[64], out[128];
	size_t  p;
	/* QR set => this is a response, not a query, and must be ignored. */
	put_header(buf, 0x2222u, 0x8180u, 1);
	p = CAPTIVE_DNS_HEADER_LEN;
	p += put_question(buf, p, "example.com", TEST_QTYPE_A, CAPTIVE_DNS_CLASS_IN);
	captive_dns_query_t q = captive_dns_parse_query(buf, p);
	TEST_ASSERT_EQUAL(CAPTIVE_DNS_KIND_NONE, q.kind);
	TEST_ASSERT_EQUAL_UINT32_MESSAGE(
		0u, captive_dns_build_response(buf, p, g_ip4, out, sizeof(out)),
		"a response packet must not be answered");
}

/* ------------------------------------------------------------------ */

static void test_a_query_produces_authoritative_answer(void)
{
	uint8_t in[128], out[128];
	captive_dns_query_t q;
	size_t  inlen, outlen, qlen, a;

	memset(in, 0, sizeof(in));
	memset(out, 0, sizeof(out));
	inlen = build_query(in, TEST_QTYPE_A, CAPTIVE_DNS_CLASS_IN);
	qlen  = inlen - CAPTIVE_DNS_HEADER_LEN;

	q = captive_dns_parse_query(in, inlen);
	TEST_ASSERT_EQUAL(CAPTIVE_DNS_KIND_A, q.kind);
	TEST_ASSERT_EQUAL_UINT16(0xBEEFu, q.txn_id);
	TEST_ASSERT_EQUAL(qlen, q.qlen);

	outlen = captive_dns_build_response(in, inlen, g_ip4, out, sizeof(out));
	TEST_ASSERT_NOT_EQUAL(0u, outlen);
	TEST_ASSERT_TRUE(outlen > inlen);

	/* Header fields. */
	TEST_ASSERT_EQUAL_UINT16(0xBEEFu, rd16(&out[0]));      /* txn id */
	TEST_ASSERT_TRUE((rd16(&out[2]) & 0x8000u) != 0u);     /* QR set */
	TEST_ASSERT_TRUE((rd16(&out[2]) & 0x0400u) != 0u);     /* AA set */
	TEST_ASSERT_EQUAL_UINT16(1u, rd16(&out[4]));           /* QDCOUNT */
	TEST_ASSERT_EQUAL_UINT16(1u, rd16(&out[6]));           /* ANCOUNT */

	/* The original question must be preserved byte-for-byte. */
	for (size_t i = 0; i < qlen; i++)
		TEST_ASSERT_EQUAL_UINT8(in[12 + i], out[12 + i]);

	/* The A answer: name pointer, type A, class IN, rdlen 4, rdata. */
	a = CAPTIVE_DNS_HEADER_LEN + qlen;
	TEST_ASSERT_EQUAL_UINT16(0xC00Cu, rd16(&out[a]));
	TEST_ASSERT_EQUAL_UINT16(TEST_QTYPE_A, rd16(&out[a + 2]));
	TEST_ASSERT_EQUAL_UINT16(CAPTIVE_DNS_CLASS_IN, rd16(&out[a + 4]));
	TEST_ASSERT_EQUAL_UINT16(60u, rd16(&out[a + 8]));  /* TTL (seconds) */
	TEST_ASSERT_EQUAL_UINT16(4u, rd16(&out[a + 10]));
	TEST_ASSERT_EQUAL_UINT8(192u, out[a + 12]);
	TEST_ASSERT_EQUAL_UINT8(0u, out[a + 13]);
	TEST_ASSERT_EQUAL_UINT8(2u, out[a + 14]);
	TEST_ASSERT_EQUAL_UINT8(1u, out[a + 15]);
}

static void test_unsupported_type_gets_empty_answer(void)
{
	/* AAAA (IPv6) and other ordinary non-A record types (MX, TXT) cannot be
	 * satisfied by the A-only captive codec, so each must yield a well-formed
	 * but answer-free (NOERROR) response that preserves the transaction id. */
	const uint16_t types[] = {TEST_QTYPE_AAAA, 15u /* MX */, 16u /* TXT */};
	uint8_t in[128], out[256];

	for (size_t i = 0; i < 3; i++) {
		captive_dns_query_t q;
		size_t inlen = build_query(in, types[i], CAPTIVE_DNS_CLASS_IN);
		size_t outlen;

		q = captive_dns_parse_query(in, inlen);
		TEST_ASSERT_EQUAL(CAPTIVE_DNS_KIND_EMPTY, q.kind);

		outlen = captive_dns_build_response(in, inlen, g_ip4, out, sizeof(out));
		TEST_ASSERT_NOT_EQUAL(0u, outlen);
		TEST_ASSERT_EQUAL_UINT16(0xBEEFu, rd16(&out[0]));
		TEST_ASSERT_EQUAL_UINT16(1u, rd16(&out[4]));   /* QDCOUNT */
		TEST_ASSERT_EQUAL_UINT16(0u, rd16(&out[6]));   /* ANCOUNT = 0 */
		TEST_ASSERT_EQUAL_UINT32(CAPTIVE_DNS_HEADER_LEN + (inlen - 12),
					 outlen);
	}
}

static void test_mixed_case_query_is_accepted(void)
{
	uint8_t in[128], out[128];
	captive_dns_query_t q;
	size_t inlen, outlen;

	/* RFC 1035 names are case-insensitive; the codec must nonetheless echo
	 * the caller's exact case in the question field. */
	put_header(in, 0x4242u, 0x0100u, 1);
	inlen = CAPTIVE_DNS_HEADER_LEN;
	inlen += put_question(in, inlen, "ExAmPlE.CoM", TEST_QTYPE_A,
	                      CAPTIVE_DNS_CLASS_IN);

	q = captive_dns_parse_query(in, inlen);
	TEST_ASSERT_EQUAL(CAPTIVE_DNS_KIND_A, q.kind);
	TEST_ASSERT_EQUAL_UINT16(0x4242u, q.txn_id);

	outlen = captive_dns_build_response(in, inlen, g_ip4, out, sizeof(out));
	TEST_ASSERT_NOT_EQUAL(0u, outlen);
	TEST_ASSERT_EQUAL_UINT16(0x4242u, rd16(&out[0]));
	/* The mixed-case question must be preserved byte-for-byte. */
	{
		uint8_t expect[64];
		size_t  elen = put_question(expect, 0, "ExAmPlE.CoM",
		                            TEST_QTYPE_A, CAPTIVE_DNS_CLASS_IN);
		TEST_ASSERT_EQUAL_UINT16(1u, rd16(&out[6]));   /* an authoritative A */
		for (size_t i = 0; i < elen; i++)
			TEST_ASSERT_EQUAL_UINT8(expect[i], out[12 + i]);
	}
}

static void test_compressed_name_query_is_accepted(void)
{
	uint8_t in[64], out[128];
	captive_dns_query_t q;
	size_t inlen, outlen;

	inlen = build_compressed_query(in, TEST_QTYPE_A);
	q = captive_dns_parse_query(in, inlen);
	TEST_ASSERT_EQUAL(CAPTIVE_DNS_KIND_A, q.kind);
	TEST_ASSERT_EQUAL_UINT16(0x5150u, q.txn_id);
	/* The QNAME is a 2-byte pointer followed by 4 bytes of type/class. */
	TEST_ASSERT_EQUAL(6u, q.qlen);

	outlen = captive_dns_build_response(in, inlen, g_ip4, out, sizeof(out));
	TEST_ASSERT_NOT_EQUAL(0u, outlen);
	TEST_ASSERT_EQUAL_UINT16(0x5150u, rd16(&out[0]));
	TEST_ASSERT_EQUAL_UINT16(1u, rd16(&out[4]));   /* QDCOUNT */
	TEST_ASSERT_EQUAL_UINT16(1u, rd16(&out[6]));   /* ANCOUNT */
	/* Header + echoed question + one 16-byte A answer. */
	TEST_ASSERT_EQUAL_UINT32(CAPTIVE_DNS_HEADER_LEN + 6u +
	                         TEST_ANSWER_LEN, outlen);
	/* The pointer-encoded question must be echoed verbatim. */
	for (size_t i = 0; i < 6; i++)
		TEST_ASSERT_EQUAL_UINT8(in[12 + i], out[12 + i]);
}

static void test_malformed_label_is_rejected(void)
{
	uint8_t in[64], out[64];
	size_t p;

	/* A label whose length octet claims more bytes than the packet actually
	 * holds is malformed and must never be answered. */
	put_header(in, 0x3333u, 0x0100u, 1);
	p = CAPTIVE_DNS_HEADER_LEN;
	in[p++] = 0x40u;    /* label length 64 -- far beyond the available bytes */
	in[p++] = 'e';
	in[p++] = 'x';
	captive_dns_query_t q = captive_dns_parse_query(in, p);
	TEST_ASSERT_EQUAL(CAPTIVE_DNS_KIND_NONE, q.kind);
	TEST_ASSERT_EQUAL_UINT32(
		0u, captive_dns_build_response(in, p, g_ip4, out, sizeof(out)));
}

static void test_undersized_output_buffer_is_refused(void)
{
	uint8_t in[128], out[30];
	size_t  inlen = build_query(in, TEST_QTYPE_A, CAPTIVE_DNS_CLASS_IN);
	/* The real A response is longer than 30 bytes, so it must be refused. */
	TEST_ASSERT_EQUAL_UINT32_MESSAGE(
		0u, captive_dns_build_response(in, inlen, g_ip4, out, sizeof(out)),
		"an undersized output buffer must be refused");
}

static void test_null_input_is_ignored(void)
{
	uint8_t out[128];
	captive_dns_query_t q = captive_dns_parse_query(NULL, 0);
	TEST_ASSERT_EQUAL(CAPTIVE_DNS_KIND_NONE, q.kind);
	TEST_ASSERT_EQUAL_UINT32(
		0u, captive_dns_build_response(NULL, 0, g_ip4, out, sizeof(out)));
}

/* ------------------------------------------------------------------ */

static void captive_dns_server_tests_run(void)
{
	RUN_TEST(test_truncated_packet_is_rejected);
	RUN_TEST(test_oversized_packet_is_rejected);
	RUN_TEST(test_zero_questions_is_rejected);
	RUN_TEST(test_multi_question_packet_is_rejected);
	RUN_TEST(test_unsupported_opcode_is_rejected);
	RUN_TEST(test_response_packet_is_ignored);
	RUN_TEST(test_a_query_produces_authoritative_answer);
	RUN_TEST(test_unsupported_type_gets_empty_answer);
	RUN_TEST(test_mixed_case_query_is_accepted);
	RUN_TEST(test_compressed_name_query_is_accepted);
	RUN_TEST(test_malformed_label_is_rejected);
	RUN_TEST(test_undersized_output_buffer_is_refused);
	RUN_TEST(test_null_input_is_ignored);
}

#ifdef ESP_PLATFORM
void app_main(void)
{
	UNITY_BEGIN();
	captive_dns_server_tests_run();
	UNITY_END();
}
#else
int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	UNITY_BEGIN();
	captive_dns_server_tests_run();
	return UNITY_END();
}
#endif