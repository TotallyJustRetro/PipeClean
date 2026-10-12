#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
typedef SOCKET NetplayTlsSocket;
#else
typedef int NetplayTlsSocket;
#endif

/* TLS is used only by the network worker thread. Read/write return a positive
   byte count, 0 when the socket must be polled, and -1 on a fatal TLS error. */
int netplay_tls_open(NetplayTlsSocket sock, const char *hostname,
                     int (*should_stop)(void));
/* Diagnostic for the most recent TLS open failure; valid until the next attempt. */
const char *netplay_tls_last_error(void);
int netplay_tls_read(NetplayTlsSocket sock, void *buffer, size_t capacity);
int netplay_tls_write(NetplayTlsSocket sock, const void *buffer, size_t length);
int netplay_tls_wants_write(void);
int netplay_tls_wait(NetplayTlsSocket sock, int want_write, int timeout_ms,
                     int (*should_stop)(void));
void netplay_tls_close(void);
