/* Verified TLS transport for PipeClean's online relay. */
#include "netplay_tls.h"

#include <SDL.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <fcntl.h>
#include <unistd.h>
#endif

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ssl.h>
#include <psa/crypto.h>
#include <mbedtls/error.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/net_sockets.h>

static mbedtls_ssl_context tls_ssl;
static mbedtls_ssl_config tls_config;
static mbedtls_ctr_drbg_context tls_drbg;
static mbedtls_entropy_context tls_entropy;
static mbedtls_x509_crt tls_ca;
static NetplayTlsSocket tls_socket;
static int tls_initialized;
static int tls_want_write;
static char tls_error_text[128];

static void tls_set_error(const char *message)
{
    snprintf(tls_error_text, sizeof tls_error_text, "%s", message ? message : "TLS error");
}

static void tls_set_mbedtls_error(const char *stage, int code)
{
    char detail[80];
    mbedtls_strerror(code, detail, sizeof detail);
    snprintf(tls_error_text, sizeof tls_error_text, "%s: %.80s (%d)", stage, detail, code);
}

const char *netplay_tls_last_error(void)
{
    return tls_error_text;
}

static int tls_stop_requested(int (*should_stop)(void))
{
    return should_stop && should_stop();
}

static int tls_set_nonblocking(NetplayTlsSocket sock)
{
#ifdef _WIN32
    u_long enabled = 1;
    return ioctlsocket(sock, FIONBIO, &enabled) == 0;
#else
    int flags = fcntl(sock, F_GETFL, 0);
    return flags >= 0 && fcntl(sock, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

static int tls_send_cb(void *context, const unsigned char *buffer, size_t length)
{
    NetplayTlsSocket sock = *(NetplayTlsSocket *)context;
    int n;
#ifdef _WIN32
    if (length > 0x7fffffffU) length = 0x7fffffffU;
    n = send(sock, (const char *)buffer, (int)length, 0);
    if (n == SOCKET_ERROR) {
        int e = WSAGetLastError();
        if (e == WSAEWOULDBLOCK || e == WSAEINTR) return MBEDTLS_ERR_SSL_WANT_WRITE;
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }
#else
    {
        ssize_t sent;
#ifdef MSG_NOSIGNAL
        sent = send(sock, buffer, length, MSG_NOSIGNAL);
#else
        sent = send(sock, buffer, length, 0);
#endif
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                return MBEDTLS_ERR_SSL_WANT_WRITE;
            return MBEDTLS_ERR_NET_SEND_FAILED;
        }
        n = (int)sent;
    }
#endif
    return n;
}

static int tls_recv_cb(void *context, unsigned char *buffer, size_t length)
{
    NetplayTlsSocket sock = *(NetplayTlsSocket *)context;
    int n;
#ifdef _WIN32
    if (length > 0x7fffffffU) length = 0x7fffffffU;
    n = recv(sock, (char *)buffer, (int)length, 0);
    if (n == SOCKET_ERROR) {
        int e = WSAGetLastError();
        if (e == WSAEWOULDBLOCK || e == WSAEINTR) return MBEDTLS_ERR_SSL_WANT_READ;
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }
#else
    {
        ssize_t received = recv(sock, buffer, length, 0);
        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                return MBEDTLS_ERR_SSL_WANT_READ;
            return MBEDTLS_ERR_NET_RECV_FAILED;
        }
        n = (int)received;
    }
#endif
    if (n == 0) return MBEDTLS_ERR_SSL_CONN_EOF;
    return n;
}

static SDL_RWops *tls_open_ca_bundle(void)
{
    SDL_RWops *rw = NULL;
#ifdef __ANDROID__
    rw = SDL_RWFromFile("assets/cacert.pem", "rb");
    if (!rw) rw = SDL_RWFromFile("cacert.pem", "rb");
#else
    char path[2048];
    char *base = SDL_GetBasePath();
    if (base) {
        if (snprintf(path, sizeof path, "%sassets/cacert.pem", base) < (int)sizeof path)
            rw = SDL_RWFromFile(path, "rb");
        SDL_free(base);
    }
    if (!rw) rw = SDL_RWFromFile("assets/cacert.pem", "rb");
#ifdef PIPECLEAN_SOURCE_ASSET_DIR
    if (!rw) {
        if (snprintf(path, sizeof path, "%s/cacert.pem", PIPECLEAN_SOURCE_ASSET_DIR) < (int)sizeof path)
            rw = SDL_RWFromFile(path, "rb");
    }
#endif
#endif
    return rw;
}

static int tls_load_ca_bundle(void)
{
    SDL_RWops *rw = tls_open_ca_bundle();
    Sint64 size;
    unsigned char *pem;
    size_t got;
    int rc;
    if (!rw) return 0;
    size = SDL_RWsize(rw);
    if (size <= 0 || size > 4 * 1024 * 1024) {
        SDL_RWclose(rw);
        return 0;
    }
    pem = (unsigned char *)SDL_malloc((size_t)size + 1);
    if (!pem) {
        SDL_RWclose(rw);
        return 0;
    }
    got = SDL_RWread(rw, pem, 1, (size_t)size);
    SDL_RWclose(rw);
    if (got != (size_t)size) {
        SDL_free(pem);
        return 0;
    }
    pem[size] = 0;
    rc = mbedtls_x509_crt_parse(&tls_ca, pem, (size_t)size + 1);
    SDL_free(pem);
    return rc == 0;
}

static int tls_wait_internal(NetplayTlsSocket sock, int want_write, int timeout_ms,
                             int (*should_stop)(void))
{
    while (!tls_stop_requested(should_stop)) {
        fd_set read_set, write_set, error_set;
        struct timeval timeout;
        int ready;
        FD_ZERO(&read_set);
        FD_ZERO(&write_set);
        FD_ZERO(&error_set);
        if (want_write) FD_SET(sock, &write_set);
        else FD_SET(sock, &read_set);
        FD_SET(sock, &error_set);
        timeout.tv_sec = timeout_ms / 1000;
        timeout.tv_usec = (timeout_ms % 1000) * 1000;
#ifdef _WIN32
        ready = select(0, &read_set, &write_set, &error_set, &timeout);
#else
        ready = select(sock + 1, &read_set, &write_set, &error_set, &timeout);
#endif
        if (ready < 0) {
#ifndef _WIN32
            if (errno == EINTR) continue;
#endif
            return -1;
        }
        if (ready == 0) return 0;
        if (FD_ISSET(sock, &error_set)) return -1;
        return 1;
    }
    return -1;
}

static void tls_free_contexts(void)
{
    if (!tls_initialized) return;
    mbedtls_ssl_free(&tls_ssl);
    mbedtls_ssl_config_free(&tls_config);
    mbedtls_x509_crt_free(&tls_ca);
    mbedtls_ctr_drbg_free(&tls_drbg);
    mbedtls_entropy_free(&tls_entropy);
    tls_initialized = 0;
    tls_want_write = 0;
}

int netplay_tls_open(NetplayTlsSocket sock, const char *hostname,
                     int (*should_stop)(void))
{
    static const unsigned char personalization[] = "PipeClean relay";
    int rc, verify_flags;
    tls_error_text[0] = 0;
    if (!hostname || !hostname[0]) {
        tls_set_error("missing TLS hostname");
        return -1;
    }

    /* Mbed TLS 3.x TLS crypto operations can use PSA, which must be initialized
       before certificate parsing or a TLS handshake. This is idempotent. */
    {
        psa_status_t psa_rc = psa_crypto_init();
        if (psa_rc != PSA_SUCCESS) {
            snprintf(tls_error_text, sizeof tls_error_text,
                     "PSA crypto initialization failed (%d)", (int)psa_rc);
            return -1;
        }
    }

    mbedtls_ssl_init(&tls_ssl);
    mbedtls_ssl_config_init(&tls_config);
    mbedtls_ctr_drbg_init(&tls_drbg);
    mbedtls_entropy_init(&tls_entropy);
    mbedtls_x509_crt_init(&tls_ca);
    tls_initialized = 1;
    tls_socket = sock;

    if (!tls_load_ca_bundle()) {
        tls_set_error("CA bundle missing or invalid");
        tls_free_contexts();
        return -2;
    }
    rc = mbedtls_ctr_drbg_seed(&tls_drbg, mbedtls_entropy_func, &tls_entropy,
                               personalization, sizeof personalization - 1);
    if (rc != 0) {
        tls_set_mbedtls_error("TLS RNG setup", rc);
        tls_free_contexts();
        return -1;
    }
    rc = mbedtls_ssl_config_defaults(&tls_config, MBEDTLS_SSL_IS_CLIENT,
                                     MBEDTLS_SSL_TRANSPORT_STREAM,
                                     MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0) {
        tls_set_mbedtls_error("TLS config setup", rc);
        tls_free_contexts();
        return -1;
    }
    /* Render's HTTPS edge supports TLS 1.2. Pin to TLS 1.2 instead of
       allowing TLS 1.3, whose PSA key-generation path can fail with
       MBEDTLS_ERR_SSL_INTERNAL_ERROR on some Mbed TLS builds. Keep full
       certificate and hostname verification enabled. */
    mbedtls_ssl_conf_max_version(&tls_config,
                                 MBEDTLS_SSL_MAJOR_VERSION_3,
                                 MBEDTLS_SSL_MINOR_VERSION_3);
    mbedtls_ssl_conf_authmode(&tls_config, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&tls_config, &tls_ca, NULL);
    mbedtls_ssl_conf_rng(&tls_config, mbedtls_ctr_drbg_random, &tls_drbg);

    rc = mbedtls_ssl_setup(&tls_ssl, &tls_config);
    if (rc == 0) rc = mbedtls_ssl_set_hostname(&tls_ssl, hostname);
    if (rc != 0) {
        tls_set_mbedtls_error("TLS session setup", rc);
        tls_free_contexts();
        return -1;
    }
    if (!tls_set_nonblocking(sock)) {
        tls_set_error("could not set TLS socket nonblocking");
        tls_free_contexts();
        return -1;
    }
    mbedtls_ssl_set_bio(&tls_ssl, &tls_socket, tls_send_cb, tls_recv_cb, NULL);

    /* Non-blocking handshake keeps Disconnect responsive even with a dead host. */
    while (!tls_stop_requested(should_stop)) {
        rc = mbedtls_ssl_handshake(&tls_ssl);
        if (rc == 0) {
            verify_flags = (int)mbedtls_ssl_get_verify_result(&tls_ssl);
            if (verify_flags != 0) {
                tls_free_contexts();
                return -3;
            }
            tls_want_write = 0;
            return 1;
        }
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
            int wait_result = tls_wait_internal(sock, rc == MBEDTLS_ERR_SSL_WANT_WRITE, 100,
                                                should_stop);
            if (wait_result < 0) {
                tls_set_error("TLS handshake socket wait failed");
                tls_free_contexts();
                return -1;
            }
            continue;
        }
        verify_flags = (int)mbedtls_ssl_get_verify_result(&tls_ssl);
        if (verify_flags) tls_set_error("server certificate verification failed");
        else tls_set_mbedtls_error("TLS handshake", rc);
        tls_free_contexts();
        return verify_flags ? -3 : -1;
    }
    tls_set_error("TLS handshake cancelled");
    tls_free_contexts();
    return -1;
}

int netplay_tls_read(NetplayTlsSocket sock, void *buffer, size_t capacity)
{
    int rc;
    (void)sock;
    if (!tls_initialized || !buffer || !capacity) return -1;
    rc = mbedtls_ssl_read(&tls_ssl, (unsigned char *)buffer,
                          capacity > 0x7fffffffU ? 0x7fffffffU : capacity);
    if (rc > 0) {
        tls_want_write = 0;
        return rc;
    }
    if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
        tls_want_write = (rc == MBEDTLS_ERR_SSL_WANT_WRITE);
        return 0;
    }
    return -1;
}

int netplay_tls_write(NetplayTlsSocket sock, const void *buffer, size_t length)
{
    int rc;
    (void)sock;
    if (!tls_initialized || !buffer || !length) return -1;
    rc = mbedtls_ssl_write(&tls_ssl, (const unsigned char *)buffer,
                           length > 0x7fffffffU ? 0x7fffffffU : length);
    if (rc > 0) {
        tls_want_write = 0;
        return rc;
    }
    if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
        tls_want_write = (rc == MBEDTLS_ERR_SSL_WANT_WRITE);
        return 0;
    }
    return -1;
}

int netplay_tls_wants_write(void)
{
    return tls_want_write;
}

int netplay_tls_wait(NetplayTlsSocket sock, int want_write, int timeout_ms,
                     int (*should_stop)(void))
{
    return tls_wait_internal(sock, want_write, timeout_ms, should_stop);
}

void netplay_tls_close(void)
{
    tls_free_contexts();
}
