/*
 * Captive (wildcard) DNS server packet codec.
 *
 * Implements bounded parsing of single-question DNS queries and generation of
 * corresponding responses, without opening any socket or touching the network
 * stack. The caller feeds an already-received query buffer and gets back a
 * fully-formed response (or a "do not respond" marker) plus the encoded length
 * to transmit.
 *
 * The codec only depends on the bounded Mongoose DNS parsing API and portable
 * types, so it compiles on POSIX and ESP platforms alike. It never registers a
 * listener and never reads from or writes to a socket.
 */

#ifndef CAPTIVE_DNS_SERVER_H
#define CAPTIVE_DNS_SERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* DNS-over-UDP message size cap. Anything larger is rejected as oversized and
 * never granted input or output buffer space. */
#define CAPTIVE_DNS_MAX_PACKET 512u
#define CAPTIVE_DNS_HEADER_LEN 12u

/* Internet (IN) class used by ordinary DNS queries. */
#define CAPTIVE_DNS_CLASS_IN 1u

/* How a parsed captive DNS query should be answered. */
typedef enum {
  /* Do not respond: the packet is malformed, truncated, oversized, carries
   * more than one question, uses an unsupported opcode, or is not a query. */
  CAPTIVE_DNS_KIND_NONE = 0,
  /* Reply with an authoritative A record for the configured IPv4 address. */
  CAPTIVE_DNS_KIND_A,
  /* Reply with a well-formed but answer-free (NOERROR) response. Used for
	 * valid queries whose record type the captive server cannot satisfy. */
  CAPTIVE_DNS_KIND_EMPTY
} captive_dns_kind_t;

/* Result of parsing a single-question captive DNS query. */
typedef struct {
  captive_dns_kind_t kind;  /* How the caller should answer the query. */
  uint16_t           txn_id; /* Preserved transaction id from the header. */
  size_t             qlen;   /* Encoded length of the echoed question RR. */
} captive_dns_query_t;

/**
 * @brief Parse a single-question DNS query from `buf[0..len)`.
 *
 * Uses the bounded Mongoose DNS parsing API. Does not open a socket.
 *
 * @param[in] buf Input packet (must be non-NULL).
 * @param[in] len Number of valid bytes in `buf`.
 * @return A filled-in descriptor. `kind == CAPTIVE_DNS_KIND_NONE` signals that
 *         the packet is unusable and must not receive a response. Otherwise
 *         `kind` tells how to answer, `txn_id` holds the preserved transaction
 *         ID, and `qlen` is the encoded question length for the response.
 */
captive_dns_query_t captive_dns_parse_query(const uint8_t *buf, size_t len);

/**
 * @brief Build a captive DNS response for a received query.
 *
 * The response preserves the transaction ID and the original question, and
 * carries an authoritative IPv4 A answer (or an empty answer for unsupported
 * record types). The function never opens a socket.
 *
 * @param[in]  query  The raw received DNS query packet.
 * @param[in]  len    Length of `query`.
 * @param[in]  ip4    Four-byte big-endian IPv4 address for A answers.
 * @param[out] out    Fixed-size output buffer.
 * @param[in]  out_cap Capacity of `out`.
 *
 * @return Number of bytes written to `out`, or 0 if no response should be
 *         produced (malformed/truncated/oversized/multi-question/unsupported
 *         opcode input, or an output buffer that is too small).
 */
size_t captive_dns_build_response(const uint8_t *query, size_t len,
                                const uint8_t ip4[4], uint8_t *out,
                                size_t out_cap);

/* ------------------------------------------------------------------ */
/* Reusable captive DNS server lifecycle.
 *
 * Wraps the packet codec in a Mongoose UDP service that owns exactly one
 * listener.  The service rides the shared Mongoose process (see
 * MongooseProcess_Invoke()): start() binds the listener on the poll thread and
 * stop() closes only the listener that this service created, leaving the
 * shared process and any other listeners untouched.
 */

/* Maximum length of the configured UDP listen URL. */
#define CAPTIVE_DNS_URL_MAX_LEN 128u

/**
 * @brief Configure the UDP listen URL used by the captive DNS service.
 *
 * The URL must be a Mongoose UDP listen URL (for example
 * "udp://0.0.0.0:53").  It is stored and applied the next time the service is
 * started.  Changing the URL while the service is running is not allowed.
 *
 * @param[in] url Non-NULL, non-empty UDP URL.
 * @return true when accepted, false when NULL/empty/too long, or when the
 *         service is already running.
 */
bool captive_dns_server_set_bind_url(const char *url);

/**
 * @brief Configure the IPv4 address returned in captive A answers.
 *
 * The default is 0.0.0.0; call this before start() to answer queries with a
 * useful captive portal address.
 *
 * @param[in] ip4 Four-byte big-endian IPv4 address.
 */
void captive_dns_server_set_ip4(const uint8_t ip4[4]);

/**
 * @brief Start the captive DNS service (idempotent).
 *
 * Ensures the shared Mongoose process is running, then creates the UDP
 * listener on the Mongoose poll thread via MongooseProcess_Invoke().  Calling
 * again while already running is a safe no-op that returns true.
 *
 * @return true when the listener is bound (started now, or already running),
 *         false if the Mongoose process cannot be started, no bind URL has
 *         been configured, or the UDP bind fails (e.g. the port is in use).
 */
bool captive_dns_server_start(void);

/**
 * @brief Stop the captive DNS service (idempotent, single-owner).
 *
 * Closes only the DNS listener owned by this service.  The listener close
 * callback runs on the Mongoose poll thread and completes before this function
 * returns.  The shared Mongoose process and any other listeners are left
 * running.  Safe to call when the service is not running.  Concurrent stop
 * callers are serialized: a caller arriving while another stop is in flight
 * waits for it to finish and then receives the same completed result.
 *
 * @return true when the listener is stopped (now, already, or released by a
 *         Mongoose teardown), false when the owned listener could not be
 *         closed while the shared Mongoose process is still running.
 */
bool captive_dns_server_stop(void);

/**
 * @brief Return whether the captive DNS service is currently running.
 */
bool captive_dns_server_is_running(void);

#endif    /* CAPTIVE_DNS_SERVER_H */