#undef MG_ARCH
#define MG_ARCH MG_ARCH_UNIX

/* Allow build system to override TLS backend (e.g. MG_TLS_MBED via -D flag).
 * Fall back to none if nothing is specified. */
#ifndef MG_TLS
#define MG_TLS MG_TLS_NONE
#endif