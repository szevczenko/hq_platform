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

captive_dns_query_t CaptiveDns_ParseQuery(const uint8_t *buf, size_t len)
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

size_t CaptiveDns_BuildResponse(const uint8_t *query, size_t len,
                                const uint8_t ip4[4], uint8_t *out,
                                size_t out_cap)
{
  captive_dns_query_t q   = CaptiveDns_ParseQuery(query, len);
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
  bool bound;                             /* A bind URL has been configured */
  bool running;                          /* Service is currently listening */
  uint8_t ip4[4];                        /* Captive A answer IPv4 address */
  struct mg_connection *nc;             /* Owned DNS listener (poll thread) */
} captive_dns_service_t;

static captive_dns_service_t s_svc;
static uint8_t s_resp[CAPTIVE_DNS_MAX_PACKET];
static osal_mutex_id_t s_mutex;
static bool s_mutex_ready;

/* Lazily create the state mutex.  Idempotent; safe if recreated after the
 * service is stopped.  Returns false only if the OSAL call fails. */
static bool dns_ensure_mutex(void)
{
  if (s_mutex_ready) return true;
  osal_mutex_id_t mtx;
  if (osal_mutex_create(&mtx, "captive_dns") != OSAL_SUCCESS) return false;
  s_mutex = mtx;
  s_mutex_ready = true;
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

  n = CaptiveDns_BuildResponse(nc->recv.buf, nc->recv.len, s_svc.ip4,
                               s_resp, sizeof(s_resp));
  if (n != 0) (void)mg_send(nc, s_resp, n);

  /* Datagram fully consumed; make room for the next one. */
  nc->recv.len = 0;
}

/* Creates the DNS listener.  Runs on the Mongoose poll thread. */
static void dns_listener_start_cb(struct mg_mgr *mgr, void *user)
{
  (void)user;
  if (s_svc.nc != NULL) return;  /* Already listening; keep the existing one. */
  s_svc.nc = mg_listen(mgr, s_svc.bind_url, dns_ev_handler, NULL);
}

/* Closes only the DNS listener owned by this service.  Runs on the Mongoose
 * poll thread.  Leaves the shared process (and all other listeners) intact. */
static void dns_listener_stop_cb(struct mg_mgr *mgr, void *user)
{
  (void)mgr;
  (void)user;
  if (s_svc.nc != NULL) {
    mg_close_conn(s_svc.nc);
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
    s_svc.bound = true;
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
  bool bound;

  if (!dns_ensure_mutex()) return false;

  (void)osal_mutex_take(s_mutex);
  if (s_svc.running) {
    (void)osal_mutex_give(s_mutex);
    return true;  /* Idempotent: already running. */
  }
  bound = s_svc.bound;
  (void)osal_mutex_give(s_mutex);

  if (!bound) return false;  /* No bind URL configured yet. */

  /* The DNS service rides the shared Mongoose process. */
  MongooseProcess_Init();
  if (!MongooseProcess_IsRunning()) return false;

  if (!MongooseProcess_Invoke(dns_listener_start_cb, NULL,
                              CAPTIVE_DNS_INVOKE_TIMEOUT_MS))
    return false;

  (void)osal_mutex_take(s_mutex);
  bound = (s_svc.nc != NULL);
  if (bound) s_svc.running = true;
  (void)osal_mutex_give(s_mutex);
  return bound;  /* false reports a bind failure to the caller */
}

void captive_dns_server_stop(void)
{
  bool was_running;

  if (!dns_ensure_mutex()) return;

  (void)osal_mutex_take(s_mutex);
  was_running = s_svc.running;
  s_svc.running = false;
  (void)osal_mutex_give(s_mutex);

  if (!was_running) return;  /* Idempotent: nothing to close. */
  if (!MongooseProcess_IsRunning()) return;  /* Process already gone. */

  MongooseProcess_Invoke(dns_listener_stop_cb, NULL,
                         CAPTIVE_DNS_INVOKE_TIMEOUT_MS);
}
