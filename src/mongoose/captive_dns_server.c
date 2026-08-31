/*
 * Captive (wildcard) DNS server packet codec and UDP service.
 *
 * See captive_dns_server.h for the public contract. The packet codec is a
 * pure, bounded encode/decode layer with no socket APIs.  The lower half of
 * this file adds a reusable UDP listener service that binds a Mongoose UDP
 * URL on the shared Mongoose poll thread and echoes captive A answers.
 */

#include "captive_dns_server.h"

#include <string.h>

#include "mongoose.h"
#include "mongoose_process.h"
#include "osal_mutex.h"

/* DNS header flag bits (RFC 1035 section 4.1.1). */
#define CAPTIVE_DNS_FLAG_QR 0x8000u    /* Query / response */
#define CAPTIVE_DNS_FLAG_AA 0x0400u    /* Authoritative answer */
#define CAPTIVE_DNS_FLAG_RA 0x0080u    /* Recursion available */
#define CAPTIVE_DNS_OPCODE_MASK 0x7800u /* Opcode occupies bits 11..14 */
#define CAPTIVE_DNS_OPCODE_STD 0x0000u /* Standard query (opcode 0) */

/* TTL (in seconds) advertised in the synthesised A record. */
#define CAPTIVE_DNS_TTL 60u

/* Size of the fixed part of an answer RR: 2 (name pointer) + 2 (type) +
 * 2 (class) + 4 (ttl) + 2 (rdlength) + 4 (rdata). */
#define CAPTIVE_DNS_ANSWER_LEN 16u
/* Name compression pointer back to the question name at offset 12. */
#define CAPTIVE_DNS_NAME_POINTER 0xC00Cu

static uint16_t be16(const uint8_t *p)
{
  return (uint16_t)(((uint16_t) p[0] << 8) | p[1]);
}

static void put16(uint8_t *p, uint16_t v)
{
  p[0] = (uint8_t) (v >> 8);
  p[1] = (uint8_t) (v & 0xFFu);
}

captive_dns_query_t captive_dns_parse_query(const uint8_t *buf, size_t len)
{
  captive_dns_query_t r     = {CAPTIVE_DNS_KIND_NONE, 0, 0};
  struct mg_dns_header  hdr;
  struct mg_dns_rr      rr;
  uint16_t              flags;
  size_t                n;

  if (buf == NULL || len < CAPTIVE_DNS_HEADER_LEN) return r;  /* truncated */
  if (len > CAPTIVE_DNS_MAX_PACKET) return r;                 /* oversized */

  memcpy(&hdr, buf, sizeof(hdr));
  r.txn_id = mg_ntohs(hdr.txnid);

  flags = mg_ntohs(hdr.flags);
  /* Must be a query (QR clear) using the standard query opcode. */
  if ((flags & CAPTIVE_DNS_FLAG_QR) != 0) return r;
  if ((flags & CAPTIVE_DNS_OPCODE_MASK) != CAPTIVE_DNS_OPCODE_STD) return r;

  /* The codec handles exactly one question. */
  if (mg_ntohs(hdr.num_questions) != 1) return r;

  /* Bounded parse of the single question record. */
  if ((n = mg_dns_parse_rr(buf, len, CAPTIVE_DNS_HEADER_LEN, true, &rr)) == 0)
    return r;  /* malformed or truncated question */

  r.qlen = n;

  if (rr.aclass == CAPTIVE_DNS_CLASS_IN && rr.atype == MG_DNS_RTYPE_A)
    r.kind = CAPTIVE_DNS_KIND_A;
  else
    r.kind = CAPTIVE_DNS_KIND_EMPTY;

  return r;
}

size_t captive_dns_build_response(const uint8_t *query, size_t len,
                                const uint8_t ip4[4], uint8_t *out,
                                size_t out_cap)
{
  captive_dns_query_t q   = captive_dns_parse_query(query, len);
  size_t              pos = 0;
  size_t              need;

  if (q.kind == CAPTIVE_DNS_KIND_NONE || out == NULL) return 0;

  /* Header + echoed question, plus an A answer when applicable. */
  need = CAPTIVE_DNS_HEADER_LEN + q.qlen;
  if (q.kind == CAPTIVE_DNS_KIND_A) need += CAPTIVE_DNS_ANSWER_LEN;
  if (out_cap < need) return 0;

  /* Header: preserve the transaction id; QR | AA | RA | opcode 0. */
  put16(&out[0], q.txn_id);
  put16(&out[2], CAPTIVE_DNS_FLAG_QR | CAPTIVE_DNS_FLAG_AA | CAPTIVE_DNS_FLAG_RA);
  put16(&out[4], 1);                              /* QDCOUNT */
  put16(&out[6], q.kind == CAPTIVE_DNS_KIND_A ? 1 : 0); /* ANCOUNT */
  put16(&out[8], 0);                              /* NSCOUNT */
  put16(&out[10], 0);                             /* ARCOUNT */

  pos = CAPTIVE_DNS_HEADER_LEN;
  /* Echo the original question verbatim (preserves type, class and name). */
  memcpy(&out[pos], &query[CAPTIVE_DNS_HEADER_LEN], q.qlen);
  pos += q.qlen;

  if (q.kind == CAPTIVE_DNS_KIND_A) {
    /* Name: pointer to the echoed question name at offset 12. */
    put16(&out[pos], CAPTIVE_DNS_NAME_POINTER);
    put16(&out[pos + 2], MG_DNS_RTYPE_A);         /* TYPE A */
    put16(&out[pos + 4], CAPTIVE_DNS_CLASS_IN);   /* CLASS IN */
    out[pos + 6] = 0;                             /* TTL (high) */
    out[pos + 7] = 0;
    out[pos + 8] = (uint8_t) (CAPTIVE_DNS_TTL >> 8);
    out[pos + 9] = (uint8_t) (CAPTIVE_DNS_TTL & 0xFFu);
    put16(&out[pos + 10], 4);                   /* RDLENGTH */
    out[pos + 12] = ip4[0];                   /* IPv4 rdata */
    out[pos + 13] = ip4[1];
    out[pos + 14] = ip4[2];
    out[pos + 15] = ip4[3];
    pos += CAPTIVE_DNS_ANSWER_LEN;
  }

  return pos;
}

/* ------------------------------------------------------------------ */
/* Captive DNS UDP service lifecycle.                                  */
/* ------------------------------------------------------------------ */

/* How long to wait for the listener create/close callback to complete on the
 * shared Mongoose poll thread. */
#define CAPTIVE_DNS_INVOKE_TIMEOUT_MS 2000u

typedef struct {
  char bind_url[CAPTIVE_DNS_URL_MAX_LEN]; /* Configured UDP listen URL */
  bool bind_url_set;                      /* A bind URL has been configured */
  bool running;                           /* Service is currently listening */
  uint8_t ip4[4];                         /* Captive A answer IPv4 address */
  struct mg_connection *nc;              /* Owned DNS listener (poll thread) */
} captive_dns_service_t;

static captive_dns_service_t s_svc;
static uint8_t s_resp[CAPTIVE_DNS_MAX_PACKET];
static osal_mutex_id_t s_mutex;
static bool s_mutex_ready;
/* Dedicated lifecycle mutex. Serializes the whole start()/stop() operations so
 * concurrent callers cannot both operate on the owned listener state, and a
 * stop caller arriving during another stop waits for it to finish and then
 * observes the completed result (running == false). */
static osal_mutex_id_t s_lifecycle_mutex;
static bool s_lifecycle_ready;

/* Lazily create the state and lifecycle mutexes.  Idempotent; safe if
 * recreated after the service is stopped.  Returns false only if an OSAL call
 * fails. */
static bool dns_ensure_mutex(void)
{
  if (s_mutex_ready && s_lifecycle_ready) return true;

  if (!s_mutex_ready) {
    osal_mutex_id_t mtx;
    if (osal_mutex_create(&mtx, "captive_dns") != OSAL_SUCCESS) return false;
    s_mutex = mtx;
    s_mutex_ready = true;
  }

  if (!s_lifecycle_ready) {
    osal_mutex_id_t mtx;
    if (osal_mutex_create(&mtx, "captive_dns_lc") != OSAL_SUCCESS) return false;
    s_lifecycle_mutex = mtx;
    s_lifecycle_ready = true;
  }

  return true;
}

/* Parses and answers a single received DNS datagram.  Runs on the Mongoose
 * poll thread for the UDP listener owned by this service. */
static void dns_ev_handler(struct mg_connection *nc, int ev, void *ev_data)
{
  size_t n;

  (void)ev_data;
  if (ev != MG_EV_READ) return;
  if (nc->recv.len == 0) return;

  n = captive_dns_build_response(nc->recv.buf, nc->recv.len, s_svc.ip4,
                               s_resp, sizeof(s_resp));
  if (n != 0) (void)mg_send(nc, s_resp, n);

  /* Datagram fully consumed; make room for the next one. */
  nc->recv.len = 0;
}

/* Result of a DNS listener bind carried out on the Mongoose poll thread. The
 * poll thread writes the boolean; the caller reads it only after
 * MongooseProcess_Invoke() returns, so the owned listener pointer itself is
 * never shared across threads. */
typedef struct {
  bool bound;
} dns_listener_bind_result_t;

/* Creates the DNS listener.  Runs exclusively on the Mongoose poll thread, so
 * it is the only place (besides dns_listener_stop_cb) that may read or write
 * the owned s_svc.nc pointer. */
static void dns_listener_start_cb(struct mg_mgr *mgr, void *user)
{
  dns_listener_bind_result_t *result = (dns_listener_bind_result_t *) user;

  if (result != NULL) result->bound = false;
  if (s_svc.nc != NULL) {
    /* Already listening; keep the existing one. */
    if (result != NULL) result->bound = true;
    return;
  }
  s_svc.nc = mg_listen(mgr, s_svc.bind_url, dns_ev_handler, NULL);
  if (result != NULL) result->bound = (s_svc.nc != NULL);
}

/* Closes only the DNS listener owned by this service.  Runs on the Mongoose
 * poll thread and completes before captive_dns_server_stop() returns (the
 * invocation is per-request synchronous).  Leaves the shared process (and all
 * other listeners) intact. */
static void dns_listener_stop_cb(struct mg_mgr *mgr, void *user)
{
  (void)mgr;
  (void)user;
  if (s_svc.nc != NULL) {
    /* Ask the poll thread to release the listener. Mongoose closes the
     * underlying socket only when close_conn() runs in the poll loop after
     * is_closing is set; calling mg_close_conn() here would free the
     * connection record but leak the listening fd. */
    s_svc.nc->is_closing = 1;
    s_svc.nc = NULL;
  }
}

void captive_dns_server_set_ip4(const uint8_t ip4[4])
{
  int i;

  if (!dns_ensure_mutex()) return;
  if (ip4 == NULL) return;

  (void)osal_mutex_take(s_mutex);
  for (i = 0; i < 4; i++) s_svc.ip4[i] = ip4[i];
  (void)osal_mutex_give(s_mutex);
}

bool captive_dns_server_set_bind_url(const char *url)
{
  size_t len;
  bool   ok = false;

  if (!dns_ensure_mutex() || url == NULL) return false;

  len = strlen(url);
  if (len == 0 || len >= CAPTIVE_DNS_URL_MAX_LEN) return false;

  (void)osal_mutex_take(s_mutex);
  if (s_svc.running) {
    /* Refuse to change the URL while the listener is live. */
  } else {
    memcpy(s_svc.bind_url, url, len);
    s_svc.bind_url[len] = '\0';
    s_svc.bind_url_set = true;
    ok = true;
  }
  (void)osal_mutex_give(s_mutex);
  return ok;
}

bool captive_dns_server_is_running(void)
{
  bool running = false;

  if (!dns_ensure_mutex()) return false;
  (void)osal_mutex_take(s_mutex);
  running = s_svc.running;
  (void)osal_mutex_give(s_mutex);
  return running;
}

bool captive_dns_server_start(void)
{
  bool url_set;

  if (!dns_ensure_mutex()) return false;

  /* Serialize the entire start() with stop(): single-owner lifecycle. */
  (void)osal_mutex_take(s_lifecycle_mutex);

  (void)osal_mutex_take(s_mutex);
  if (s_svc.running) {
    (void)osal_mutex_give(s_mutex);
    (void)osal_mutex_give(s_lifecycle_mutex);
    return true;  /* Idempotent: already running. */
  }
  url_set = s_svc.bind_url_set;
  (void)osal_mutex_give(s_mutex);

  if (!url_set) {
    (void)osal_mutex_give(s_lifecycle_mutex);
    return false;  /* No bind URL configured yet. */
  }

  /* The DNS service rides the shared Mongoose process. */
  MongooseProcess_Init();
  if (!MongooseProcess_IsRunning()) {
    (void)osal_mutex_give(s_lifecycle_mutex);
    return false;
  }

  /* Bind on the poll thread. The callback reports the outcome through the
   * caller-local result, so start() never reads the owned listener pointer. */
  {
    dns_listener_bind_result_t bind_result;

    if (!MongooseProcess_Invoke(dns_listener_start_cb, &bind_result,
                                CAPTIVE_DNS_INVOKE_TIMEOUT_MS) ||
        !bind_result.bound) {
      (void)osal_mutex_give(s_lifecycle_mutex);
      return false;  /* Invocation failed or the UDP bind was refused. */
    }
  }

  (void)osal_mutex_take(s_mutex);
  s_svc.running = true;
  (void)osal_mutex_give(s_mutex);
  (void)osal_mutex_give(s_lifecycle_mutex);
  return true;
}

bool captive_dns_server_stop(void)
{
  bool was_running;
  bool ok = true;

  if (!dns_ensure_mutex()) return false;

  /* Serialize the whole stop() with start(). A caller arriving while another
   * stop is in flight waits here and then observes running == false below,
   * receiving the same completed result. */
  (void)osal_mutex_take(s_lifecycle_mutex);

  (void)osal_mutex_take(s_mutex);
  was_running = s_svc.running;
  (void)osal_mutex_give(s_mutex);

  if (was_running && MongooseProcess_IsRunning()) {
    /* Close on the poll thread and confirm the close callback completed. If
     * the owned listener cannot be closed while Mongoose is still running,
     * the caller is told so it can report a lifecycle error. */
    ok = MongooseProcess_Invoke(dns_listener_stop_cb, NULL,
                                CAPTIVE_DNS_INVOKE_TIMEOUT_MS);
  }
  if (ok) {
    (void)osal_mutex_take(s_mutex);
    s_svc.running = false;
    (void)osal_mutex_give(s_mutex);
  }
  /* was_running == false: nothing to close (idempotent stop).  Mongoose not
   * running: its teardown has already released every listener.  Both are
   * successful stops. */
  (void)osal_mutex_give(s_lifecycle_mutex);
  return ok;
}
