/*
 * PipeClean relay client: bounded line protocol with one SDL worker thread.
 */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#endif
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "netplay.h"
#include "games.h"
#include "netplay_tls.h"
#include <mbedtls/base64.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/sha1.h>

#ifdef _WIN32
typedef SOCKET NetSocket;
#define NET_INVALID_SOCKET INVALID_SOCKET
#define net_close_socket(s) closesocket(s)
#define net_shutdown_socket(s) shutdown((s), SD_BOTH)
#else
typedef int NetSocket;
#define NET_INVALID_SOCKET (-1)
#define net_close_socket(s) close(s)
#define net_shutdown_socket(s) shutdown((s), SHUT_RDWR)
#endif

static SDL_mutex *np_mutex;
static SDL_mutex *np_send_mutex;
static SDL_cond *np_cond;
static SDL_Thread *np_thread;
static NetSocket np_socket = NET_INVALID_SOCKET;
static int np_state = NETPLAY_OFFLINE;
static int np_running, np_stop, np_is_host;
static char np_status[128] = "Not connected";
static char np_code[16], np_peer[32];
static char np_endpoint[160], np_name[32], np_join_code[16];
static int np_selected_game = -1;
static int np_start_request = -1;
static int np_rom_check_state = NETPLAY_ROM_UNCHECKED;
static int np_rom_check_game = -1;
static char np_rom_check_status[128] = "ROM has not been checked yet.";
static int np_local_rom_valid;
static uint32_t np_local_rom_crc;
static int np_remote_rom_valid;
static uint32_t np_remote_rom_crc;
#define NP_CHAT_CAP 8
static NetplayChatMessage np_chat[NP_CHAT_CAP];
static int np_chat_head, np_chat_count;

#define NP_INPUT_CAP 256
/* Four frames (~67 ms at 60 FPS) is a smaller latency floor; any additional
 * wait should reflect actual network delay rather than a large fixed buffer. */
#define NP_INPUT_DELAY_FRAMES 4u
#define NP_SEND_CAP 128
#define NP_GAME_TIMEOUT_MS 30000u
#define NP_FRAME_TIMEOUT_MS 15000u
#define NP_NET_MESSAGE_CAP 12288u
#define NP_SHARED_SAVE_MAX_BYTES 1000000u
#define NP_SHARED_SAVE_CHUNK_RAW 8000u
#define NP_SHARED_NAME_BYTES 32
typedef struct { uint64_t frame; uint8_t buttons, dpad, actions; } NetInput;
typedef struct { char data[NP_NET_MESSAGE_CAP + 1u]; size_t length; } NetSend;
static NetInput np_inputs[NP_INPUT_CAP];
static NetInput np_local_inputs[NP_INPUT_CAP];
static int np_input_head, np_input_count, np_input_overflow;
static NetSend np_sends[NP_SEND_CAP];
static int np_send_head, np_send_count;
static int np_tls_mode;
static int np_ws_mode, np_ws_ready;
static mbedtls_entropy_context np_ws_entropy;
static mbedtls_ctr_drbg_context np_ws_drbg;
static int np_ws_rng_ready;
static int np_local_game_pending, np_local_game, np_local_game_verified;
static int np_local_game_mismatch, np_remote_game_seen, np_remote_game;
static uint32_t np_local_crc, np_remote_crc;
static int np_game_end_received;
static uint32_t np_wait_window_total_ms, np_wait_window_max_ms, np_wait_window_slow;
static unsigned np_wait_window_samples;
static char np_shared_state_path[NETPLAY_SHARED_SLOT_COUNT][1200];
static char np_shared_marker_path[NETPLAY_SHARED_SLOT_COUNT][1200];
static char np_shared_names[NETPLAY_SHARED_SLOT_COUNT][NP_SHARED_NAME_BYTES];
static int np_shared_name_received[NETPLAY_SHARED_SLOT_COUNT];
static int np_shared_local_saved[NETPLAY_SHARED_SLOT_COUNT];
static int np_shared_peer_saved[NETPLAY_SHARED_SLOT_COUNT];
static int np_shared_local_reported[NETPLAY_SHARED_SLOT_COUNT];
static int np_shared_name_sent[NETPLAY_SHARED_SLOT_COUNT];
static int np_shared_pending[NETPLAY_SHARED_SLOT_COUNT];
static int np_shared_active_slot;
static int np_shared_load_slot = -1;
static int np_shared_selection_sent;
static uint32_t np_shared_tx_next_id = 1;
static uint32_t np_shared_tx_id, np_shared_tx_crc;
static int np_shared_tx_slot = -1, np_shared_tx_active;
static unsigned char *np_shared_rx_data;
static uint32_t np_shared_rx_id, np_shared_rx_crc, np_shared_rx_size, np_shared_rx_used;
static int np_shared_rx_slot = -1;

/* Save protocol messages are handled by the network worker, before their
 * implementations below the frame-exchange function. */
static void np_handle_save_name(const char *line);
static void np_handle_save_select(const char *line);
static void np_handle_save_status(const char *line);
static void np_handle_save_begin(const char *line);
static void np_handle_save_chunk(const char *line);
static void np_handle_save_end(const char *line);
static void np_handle_save_ack(const char *line);

static void np_lock(void) { if (np_mutex) SDL_LockMutex(np_mutex); }
static void np_unlock(void) { if (np_mutex) SDL_UnlockMutex(np_mutex); }

static void np_set_status(int state, const char *status)
{
    np_lock();
    np_state = state;
    snprintf(np_status, sizeof np_status, "%s", status ? status : "");
    if (np_cond) SDL_CondBroadcast(np_cond);
    np_unlock();
}
static void np_set_code(const char *code)
{
    np_lock();
    snprintf(np_code, sizeof np_code, "%s", code ? code : "");
    np_unlock();
}
static void np_set_peer(const char *peer)
{
    np_lock();
    snprintf(np_peer, sizeof np_peer, "%s", peer ? peer : "");
    np_unlock();
}

static void np_chat_add_locked(const char *name, const char *text)
{
    int index;
    if (np_chat_count < NP_CHAT_CAP) {
        index = (np_chat_head + np_chat_count) % NP_CHAT_CAP;
        np_chat_count++;
    } else {
        index = np_chat_head;
        np_chat_head = (np_chat_head + 1) % NP_CHAT_CAP;
    }
    snprintf(np_chat[index].name, sizeof np_chat[index].name, "%s", name ? name : "Player");
    snprintf(np_chat[index].text, sizeof np_chat[index].text, "%s", text ? text : "");
}

static void np_handle_chat_line(const char *line)
{
    const char *name, *tab;
    char clean_name[32], clean_text[96];
    size_t name_len;
    if (!line || strncmp(line, "CHAT\t", 5) != 0) return;
    name = line + 5;
    tab = strchr(name, '\t');
    if (!tab || !tab[1]) return;
    name_len = (size_t)(tab - name);
    if (name_len == 0 || name_len >= sizeof clean_name ||
        strlen(tab + 1) >= sizeof clean_text) return;
    memcpy(clean_name, name, name_len);
    clean_name[name_len] = 0;
    snprintf(clean_text, sizeof clean_text, "%s", tab + 1);
    np_lock();
    np_chat_add_locked(clean_name, clean_text);
    if (np_cond) SDL_CondBroadcast(np_cond);
    np_unlock();
}

static void np_handle_rom_result(const char *line)
{
    int game, host_ok, guest_ok;
    unsigned host_crc, guest_crc;
    char extra;
    if (sscanf(line, "ROM_RESULT\t%d\t%d\t%x\t%d\t%x%c",
               &game, &host_ok, &host_crc, &guest_ok, &guest_crc, &extra) != 5)
        return;
    np_lock();
    np_rom_check_game = game;
    np_remote_rom_valid = np_is_host ? guest_ok : host_ok;
    np_remote_rom_crc = np_is_host ? guest_crc : host_crc;
    if (game != np_selected_game) {
        np_rom_check_state = NETPLAY_ROM_MISMATCH;
        snprintf(np_rom_check_status, sizeof np_rom_check_status,
                 "Game selection changed; check ROMs again.");
    } else if (!host_ok || !guest_ok) {
        np_rom_check_state = NETPLAY_ROM_INVALID;
        snprintf(np_rom_check_status, sizeof np_rom_check_status,
                 !np_local_rom_valid ? "Your selected ROM could not be loaded." :
                 "The other player could not load the selected ROM.");
    } else if (np_local_rom_crc != np_remote_rom_crc ||
               host_crc != guest_crc) {
        np_rom_check_state = NETPLAY_ROM_MISMATCH;
        snprintf(np_rom_check_status, sizeof np_rom_check_status,
                 "ROM mismatch: both players must use the same ROM.");
    } else {
        np_rom_check_state = NETPLAY_ROM_MATCH;
        snprintf(np_rom_check_status, sizeof np_rom_check_status,
                 "ROM verified: both players have the same game.");
    }
    if (np_cond) SDL_CondBroadcast(np_cond);
    np_unlock();
}

static int np_stopping(void)
{
    int stop;
    np_lock();
    stop = np_stop;
    np_unlock();
    return stop;
}

static int np_parse_endpoint(const char *endpoint, char *host, size_t host_size,
                             char *port, size_t port_size)
{
    const char *colon;
    char *end = NULL;
    long number;
    size_t len;
    if (!endpoint || !endpoint[0]) return 0;
    colon = strrchr(endpoint, ':');
    if (!colon) {
        len = strlen(endpoint);
        if (!len || len >= host_size) return 0;
        memcpy(host, endpoint, len + 1);
        snprintf(port, port_size, "443");
        return 1;
    }
    if (colon == endpoint || !colon[1]) return 0;
    len = (size_t)(colon - endpoint);
    if (!len || len >= host_size) return 0;
    memcpy(host, endpoint, len);
    host[len] = 0;
    number = strtol(colon + 1, &end, 10);
    if (!end || *end || number < 1 || number > 65535) return 0;
    snprintf(port, port_size, "%ld", number);
    return 1;
}

static void np_close_published_socket(NetSocket sock)
{
    np_lock();
    if (np_socket == sock) np_socket = NET_INVALID_SOCKET;
    np_unlock();
    net_close_socket(sock);
}

static int np_ipv4_is_private(uint32_t address)
{
    /* address is in host byte order. Only loopback, RFC1918, and link-local
       destinations are permitted while this plain-TCP prototype is LAN-only. */
    return (address & 0xFF000000u) == 0x7F000000u ||
           (address & 0xFF000000u) == 0x0A000000u ||
           (address & 0xFFF00000u) == 0xAC100000u ||
           (address & 0xFFFF0000u) == 0xC0A80000u ||
           (address & 0xFFFF0000u) == 0xA9FE0000u;
}

static int np_is_allowed_relay_address(const struct addrinfo *entry)
{
    if (!entry || !entry->ai_addr) return 0;
    if (entry->ai_family == AF_INET &&
        entry->ai_addrlen >= (size_t)sizeof(struct sockaddr_in)) {
        const struct sockaddr_in *addr = (const struct sockaddr_in *)entry->ai_addr;
        return np_ipv4_is_private(ntohl(addr->sin_addr.s_addr));
    }
#ifdef AF_INET6
    if (entry->ai_family == AF_INET6 &&
        entry->ai_addrlen >= (size_t)sizeof(struct sockaddr_in6)) {
        const struct sockaddr_in6 *addr = (const struct sockaddr_in6 *)entry->ai_addr;
        const unsigned char *b = addr->sin6_addr.s6_addr;
        int loopback = 1, mapped = 1;
        for (int i = 0; i < 15; i++) if (b[i] != 0) loopback = 0;
        if (loopback && b[15] == 1) return 1; /* ::1 */
        if ((b[0] & 0xFEu) == 0xFCu) return 1; /* Unique-local fc00::/7 */
        if (b[0] == 0xFEu && (b[1] & 0xC0u) == 0x80u) return 1; /* fe80::/10 */
        for (int i = 0; i < 10; i++) if (b[i] != 0) mapped = 0;
        if (mapped && b[10] == 0xFFu && b[11] == 0xFFu) {
            uint32_t v4 = ((uint32_t)b[12] << 24) | ((uint32_t)b[13] << 16) |
                          ((uint32_t)b[14] << 8) | (uint32_t)b[15];
            return np_ipv4_is_private(v4);
        }
    }
#endif
    return 0;
}

static NetSocket np_connect(const char *host, const char *port, int use_tls,
                             int *blocked_public_target)
{
    struct addrinfo hints, *list = NULL, *it;
    NetSocket result = NET_INVALID_SOCKET;
    int have_private_target = 0, have_public_target = 0;
    if (blocked_public_target) *blocked_public_target = 0;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    if (getaddrinfo(host, port, &hints, &list) != 0) return NET_INVALID_SOCKET;

    for (it = list; it && !np_stopping(); it = it->ai_next) {
        if (!use_tls && !np_is_allowed_relay_address(it)) {
            have_public_target = 1;
            continue;
        }
        have_private_target = 1;
        NetSocket sock = (NetSocket)socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        int connected = 0, pending = 0;
#ifdef _WIN32
        u_long nonblocking = 1;
        if (sock == NET_INVALID_SOCKET) continue;
        if (ioctlsocket(sock, FIONBIO, &nonblocking) != 0) {
            net_close_socket(sock);
            continue;
        }
#else
        int original_flags;
        if (sock == NET_INVALID_SOCKET) continue;
        original_flags = fcntl(sock, F_GETFL, 0);
        if (original_flags < 0 || fcntl(sock, F_SETFL, original_flags | O_NONBLOCK) < 0) {
            net_close_socket(sock);
            continue;
        }
#endif
        np_lock();
        if (np_stop) {
            np_unlock();
            net_close_socket(sock);
            break;
        }
        /* Publish before connect so Disconnect can interrupt the attempt. */
        np_socket = sock;
        np_unlock();

        if (connect(sock, it->ai_addr, (int)it->ai_addrlen) == 0) {
            connected = 1;
        } else {
#ifdef _WIN32
            int err = WSAGetLastError();
            pending = err == WSAEWOULDBLOCK || err == WSAEINPROGRESS;
#else
            pending = errno == EINPROGRESS || errno == EWOULDBLOCK || errno == EAGAIN;
#endif
        }

        if (pending) {
            Uint32 started = SDL_GetTicks();
            while (!np_stopping() && (Uint32)(SDL_GetTicks() - started) < 7000u) {
                fd_set write_set, error_set;
                struct timeval timeout;
                int ready;
                FD_ZERO(&write_set);
                FD_ZERO(&error_set);
                FD_SET(sock, &write_set);
                FD_SET(sock, &error_set);
                timeout.tv_sec = 0;
                timeout.tv_usec = 100000;
#ifdef _WIN32
                ready = select(0, NULL, &write_set, &error_set, &timeout);
#else
                ready = select(sock + 1, NULL, &write_set, &error_set, &timeout);
#endif
                if (ready < 0) {
#ifndef _WIN32
                    if (errno == EINTR) continue;
#endif
                    break;
                }
                if (ready == 0) continue;
                {
                    int socket_error = 0;
#ifdef _WIN32
                    int error_len = (int)sizeof socket_error;
                    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, (char *)&socket_error, &error_len) == 0 &&
                        socket_error == 0) connected = 1;
#else
                    socklen_t error_len = (socklen_t)sizeof socket_error;
                    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &socket_error, &error_len) == 0 &&
                        socket_error == 0) connected = 1;
#endif
                }
                break;
            }
        }
        if (np_stopping()) connected = 0;

        /* Restore the socket's ordinary blocking mode for line reads. */
#ifdef _WIN32
        nonblocking = 0;
        (void)ioctlsocket(sock, FIONBIO, &nonblocking);
#else
        (void)fcntl(sock, F_SETFL, original_flags);
#endif
        if (connected) {
            /* Gameplay sends tiny input messages every frame. Disable Nagle's
             * algorithm so TCP does not wait to coalesce these latency-sensitive
             * writes with later traffic. The relay already enables this on its
             * side; enable it on the client socket too. */
            {
                int no_delay = 1;
#ifdef _WIN32
                (void)setsockopt(sock, IPPROTO_TCP, TCP_NODELAY,
                                 (const char *)&no_delay, (int)sizeof no_delay);
#else
                (void)setsockopt(sock, IPPROTO_TCP, TCP_NODELAY,
                                 &no_delay, (socklen_t)sizeof no_delay);
#endif
            }
            result = sock;
            break;
        }
        np_close_published_socket(sock);
    }

    freeaddrinfo(list);
    if (result == NET_INVALID_SOCKET && !have_private_target && have_public_target &&
        blocked_public_target) *blocked_public_target = 1;
    return result;
}
static int np_send_all(NetSocket sock, const char *data, size_t length)
{
    size_t sent = 0;
    while (sent < length) {
#ifdef _WIN32
        int n = send(sock, data + sent, (int)(length - sent), 0);
#else
#ifdef MSG_NOSIGNAL
        ssize_t n = send(sock, data + sent, length - sent, MSG_NOSIGNAL);
#else
        ssize_t n = send(sock, data + sent, length - sent, 0);
#endif
#endif
        if (n <= 0) return 0;
        sent += (size_t)n;
    }
    return 1;
}
static int np_tls_write_all_raw(NetSocket sock, const char *data, size_t length)
{
    size_t sent = 0;
    while (sent < length && !np_stopping()) {
        int n = netplay_tls_write(sock, data + sent, length - sent);
        if (n > 0) {
            sent += (size_t)n;
            continue;
        }
        if (n < 0) return 0;
        {
            int waited = netplay_tls_wait(sock, netplay_tls_wants_write(), 50, np_stopping);
            if (waited < 0 && !np_stopping()) return 0;
        }
    }
    return sent == length;
}

static int np_ws_send_frame(NetSocket sock, unsigned opcode, const unsigned char *payload, size_t length)
{
    unsigned char frame[2 + 8 + 4 + NP_NET_MESSAGE_CAP];
    size_t header = 2;
    if (length > NP_NET_MESSAGE_CAP || (length && !payload) || !np_ws_rng_ready) return 0;
    frame[0] = (unsigned char)(0x80u | (opcode & 0x0fu));
    if (length <= 125u) {
        frame[1] = (unsigned char)(0x80u | (unsigned char)length);
    } else if (length <= 65535u) {
        frame[1] = 0x80u | 126u;
        frame[2] = (unsigned char)(length >> 8);
        frame[3] = (unsigned char)length;
        header = 4;
    } else return 0;
    if (mbedtls_ctr_drbg_random(&np_ws_drbg, frame + header, 4) != 0) return 0;
    for (size_t i = 0; i < length; ++i)
        frame[header + 4 + i] = payload[i] ^ frame[header + (i & 3u)];
    return np_tls_write_all_raw(sock, (const char *)frame, header + 4 + length);
}

static int np_tls_write_all(NetSocket sock, const char *data, size_t length)
{
    if (np_ws_mode && np_ws_ready)
        return np_ws_send_frame(sock, 1u, (const unsigned char *)data, length);
    return np_tls_write_all_raw(sock, data, length);
}

static int np_tls_flush_outgoing(NetSocket sock);

static int np_tls_read_exact_timeout(NetSocket sock, unsigned char *out, size_t length,
                                     Uint32 first_byte_timeout_ms)
{
    size_t used = 0;
    Uint32 started = SDL_GetTicks();
    while (used < length && !np_stopping()) {
        int n = netplay_tls_read(sock, out + used, length - used);
        if (n > 0) {
            used += (size_t)n;
            continue;
        }
        if (n < 0) return 0;
        /* Flush controller/game messages even while no incoming WS frame is ready.
           This is essential for lockstep: both peers may be waiting on each other. */
        if (!np_tls_flush_outgoing(sock)) return 0;
        if (first_byte_timeout_ms && used == 0 &&
            (Uint32)(SDL_GetTicks() - started) >= first_byte_timeout_ms)
            return 2; /* idle timeout, caller may send a keepalive ping */
        if (netplay_tls_wait(sock, netplay_tls_wants_write(), 10, np_stopping) < 0 &&
            !np_stopping()) return 0;
    }
    return used == length ? 1 : 0;
}

static int np_tls_read_exact(NetSocket sock, unsigned char *out, size_t length)
{
    return np_tls_read_exact_timeout(sock, out, length, 0) == 1;
}

static int np_websocket_handshake(NetSocket sock, const char *host)
{
    static const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    unsigned char nonce[16], digest[20], accept_b64[64];
    char key[32], expected[64], request[1024], response[2048];
    size_t key_len = 0, accept_len = 0, used = 0;
    static const unsigned char personalization[] = "PipeClean WebSocket";
    int ok = 0;

    if (np_ws_rng_ready) {
        mbedtls_ctr_drbg_free(&np_ws_drbg);
        mbedtls_entropy_free(&np_ws_entropy);
        np_ws_rng_ready = 0;
    }
    mbedtls_entropy_init(&np_ws_entropy);
    mbedtls_ctr_drbg_init(&np_ws_drbg);
    if (mbedtls_ctr_drbg_seed(&np_ws_drbg, mbedtls_entropy_func, &np_ws_entropy,
                              personalization, sizeof personalization - 1) != 0 ||
        mbedtls_ctr_drbg_random(&np_ws_drbg, nonce, sizeof nonce) != 0) {
        mbedtls_ctr_drbg_free(&np_ws_drbg);
        mbedtls_entropy_free(&np_ws_entropy);
        return 0;
    }
    np_ws_rng_ready = 1;
    if (mbedtls_base64_encode((unsigned char *)key, sizeof key, &key_len,
                              nonce, sizeof nonce) != 0 || key_len >= sizeof key)
        return 0;
    key[key_len] = 0;
    {
        char combined[128];
        int n = snprintf(combined, sizeof combined, "%s%s", key, guid);
        if (n < 0 || (size_t)n >= sizeof combined ||
            mbedtls_sha1((const unsigned char *)combined, (size_t)n, digest) != 0)
            return 0;
    }
    if (mbedtls_base64_encode(accept_b64, sizeof accept_b64, &accept_len,
                              digest, sizeof digest) != 0 || accept_len >= sizeof expected)
        return 0;
    memcpy(expected, accept_b64, accept_len);
    expected[accept_len] = 0;

    {
        int n = snprintf(request, sizeof request,
            "GET /relay HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: %s\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "\r\n", host, key);
        if (n < 0 || (size_t)n >= sizeof request ||
            !np_tls_write_all_raw(sock, request, (size_t)n)) return 0;
    }

    response[0] = 0;
    while (used + 1 < sizeof response && !np_stopping()) {
        unsigned char ch;
        if (!np_tls_read_exact(sock, &ch, 1)) return 0;
        response[used++] = (char)ch;
        response[used] = 0;
        if (used >= 4 && memcmp(response + used - 4, "\r\n\r\n", 4) == 0)
            break;
    }
    if (used + 1 >= sizeof response || np_stopping()) return 0;
    if (!strstr(response, "101 Switching Protocols") ||
        !strstr(response, "Upgrade: websocket")) return 0;
    {
        char header[96];
        snprintf(header, sizeof header, "Sec-WebSocket-Accept: %s", expected);
        if (!strstr(response, header)) return 0;
    }
    ok = 1;
    return ok;
}

static int np_ws_read_line(NetSocket sock, char *out, size_t cap)
{
    for (;;) {
        unsigned char header[2], extended[8], mask[4], payload[NP_NET_MESSAGE_CAP];
        uint64_t length;
        unsigned opcode, fin, masked;
        int header_read = np_tls_read_exact_timeout(sock, header, sizeof header, 30000u);
        if (header_read == 2) {
            static const unsigned char application_ping[] = "PING\n";
            unsigned char ping[4];
            if (!np_ws_rng_ready ||
                mbedtls_ctr_drbg_random(&np_ws_drbg, ping, sizeof ping) != 0 ||
                !np_ws_send_frame(sock, 9u, ping, sizeof ping) ||
                !np_ws_send_frame(sock, 1u, application_ping, sizeof application_ping - 1))
                return 0;
            continue;
        }
        if (header_read != 1) return 0;
        fin = header[0] >> 7;
        opcode = header[0] & 0x0fu;
        masked = header[1] >> 7;
        length = header[1] & 0x7fu;
        if (length == 126) {
            if (!np_tls_read_exact(sock, extended, 2)) return 0;
            length = ((uint64_t)extended[0] << 8) | extended[1];
        } else if (length == 127) {
            if (!np_tls_read_exact(sock, extended, 8)) return 0;
            length = 0;
            for (int i = 0; i < 8; ++i) length = (length << 8) | extended[i];
        }
        if (length > sizeof payload || length + 1 > cap || length > 2048) return 0;
        if (masked && !np_tls_read_exact(sock, mask, sizeof mask)) return 0;
        if (length && !np_tls_read_exact(sock, payload, (size_t)length)) return 0;
        if (masked)
            for (size_t i = 0; i < (size_t)length; ++i) payload[i] ^= mask[i & 3u];

        if (opcode == 8u) return 0; /* close */
        if (opcode == 9u) { /* respond to websocket ping with a masked pong */
            if (length > 125 || !np_ws_send_frame(sock, 10u, payload, (size_t)length)) return 0;
            continue;
        }
        if (opcode == 10u) continue; /* pong */
        if (!fin || opcode != 1u || memchr(payload, 0, (size_t)length)) return 0;
        memcpy(out, payload, (size_t)length);
        out[length] = 0;
        while (length && (out[length - 1] == '\n' || out[length - 1] == '\r'))
            out[--length] = 0;
        return 1;
    }
}

static int np_write_transport(NetSocket sock, const char *data, size_t length, int use_tls)
{
    return use_tls ? np_tls_write_all(sock, data, length) : np_send_all(sock, data, length);
}

static int np_tls_flush_outgoing(NetSocket sock)
{
    for (;;) {
        char message[NP_NET_MESSAGE_CAP + 1u];
        size_t length;
        np_lock();
        if (np_send_count <= 0) {
            np_unlock();
            return 1;
        }
        length = np_sends[np_send_head].length;
        memcpy(message, np_sends[np_send_head].data, length);
        np_send_head = (np_send_head + 1) % NP_SEND_CAP;
        np_send_count--;
        np_unlock();
        if (!np_tls_write_all(sock, message, length)) return 0;
    }
}

static int np_send_current(const char *message)
{
    NetSocket sock;
    int ok, use_tls;
    size_t length;
    if (!np_send_mutex || !np_mutex || !message) return 0;
    length = strlen(message);
    SDL_LockMutex(np_send_mutex);
    np_lock();
    sock = np_socket;
    use_tls = np_tls_mode;
    ok = (np_state == NETPLAY_CONNECTED && sock != NET_INVALID_SOCKET && !np_stop);
    if (ok && use_tls) {
        if (length >= sizeof np_sends[0].data || np_send_count >= NP_SEND_CAP) {
            np_state = NETPLAY_ERROR;
            snprintf(np_status, sizeof np_status, "Secure network send queue overflowed");
            ok = 0;
        } else {
            int tail = (np_send_head + np_send_count) % NP_SEND_CAP;
            memcpy(np_sends[tail].data, message, length);
            np_sends[tail].length = length;
            np_send_count++;
            if (np_cond) SDL_CondBroadcast(np_cond);
        }
        np_unlock();
        SDL_UnlockMutex(np_send_mutex);
        return ok;
    }
    np_unlock();
    if (ok) ok = np_send_all(sock, message, length);
    SDL_UnlockMutex(np_send_mutex);
    if (!ok) np_set_status(NETPLAY_ERROR, "Lost the relay connection while sending");
    return ok;
}

static void np_clear_input_queue_locked(void)
{
    np_input_head = 0;
    np_input_count = 0;
    np_input_overflow = 0;
    memset(np_local_inputs, 0, sizeof np_local_inputs);
}

static void np_handle_input_line(const char *line)
{
    unsigned long long frame;
    unsigned buttons, dpad, actions;
    char extra;
    if (sscanf(line, "INPUT\t%llu\t%u\t%u\t%u%c", &frame, &buttons, &dpad, &actions, &extra) != 4 ||
        buttons > 255 || dpad > 255 || actions > 255) return;
    np_lock();
    if (np_input_count >= NP_INPUT_CAP) {
        np_input_overflow = 1;
    } else {
        int tail = (np_input_head + np_input_count) % NP_INPUT_CAP;
        np_inputs[tail].frame = (uint64_t)frame;
        np_inputs[tail].buttons = (uint8_t)buttons;
        np_inputs[tail].dpad = (uint8_t)dpad;
        np_inputs[tail].actions = (uint8_t)actions;
        np_input_count++;
    }
    if (np_cond) SDL_CondBroadcast(np_cond);
    np_unlock();
}

static void np_handle_game_line(const char *line)
{
    int game;
    unsigned crc;
    if (sscanf(line, "GAME\t%d\t%x", &game, &crc) != 2) return;
    np_lock();
    np_remote_game = game;
    np_remote_crc = (uint32_t)crc;
    np_remote_game_seen = 1;
    if (np_local_game_pending) {
        np_local_game_mismatch = np_remote_game != np_local_game || np_remote_crc != np_local_crc;
        np_local_game_verified = !np_local_game_mismatch;
        if (np_local_game_mismatch)
            snprintf(np_status, sizeof np_status, "Game mismatch: both players need the same ROM");
        else
            snprintf(np_status, sizeof np_status, "Game and ROM verified");
    }
    if (np_cond) SDL_CondBroadcast(np_cond);
    np_unlock();
}

static void np_handle_game_end(void)
{
    np_lock();
    np_game_end_received = 1;
    np_local_game_pending = 0;
    np_local_game_verified = 0;
    np_local_game_mismatch = 0;
    np_remote_game_seen = 0;
    np_local_game = np_remote_game = 0;
    np_local_crc = np_remote_crc = 0;
    np_clear_input_queue_locked();
    if (np_state == NETPLAY_CONNECTED)
        snprintf(np_status, sizeof np_status, "Peer returned to the online lobby");
    if (np_cond) SDL_CondBroadcast(np_cond);
    np_unlock();
}

static int np_read_line(NetSocket sock, char *out, size_t cap, int use_tls)
{
    size_t used = 0;
    if (!out || cap < 2) return 0;
    if (use_tls && np_ws_mode) {
        if (!np_tls_flush_outgoing(sock)) return 0;
        return np_ws_read_line(sock, out, cap);
    }
    if (use_tls) {
        while (used + 1 < cap && !np_stopping()) {
            char ch;
            int n;
            if (!np_tls_flush_outgoing(sock)) return 0;
            n = netplay_tls_read(sock, &ch, 1);
            if (n > 0) {
                if (ch == '\n') { out[used] = 0; return 1; }
                if (ch != '\r') out[used++] = ch;
                continue;
            }
            if (n < 0) return 0;
            {
                int waited = netplay_tls_wait(sock, netplay_tls_wants_write(), 10, np_stopping);
                if (waited < 0 && !np_stopping()) return 0;
            }
        }
        out[used] = 0;
        return 0;
    }
    while (used + 1 < cap) {
        char ch;
#ifdef _WIN32
        int n = recv(sock, &ch, 1, 0);
#else
        ssize_t n = recv(sock, &ch, 1, 0);
#endif
        if (n <= 0) return 0;
        if (ch == '\n') { out[used] = 0; return 1; }
        if (ch != '\r') out[used++] = ch;
    }
    out[used] = 0;
    return -1;
}
static void np_safe_name(char *out, size_t cap, const char *src)
{
    size_t used = 0;
    if (!src) src = "Player";
    for (; *src && used + 1 < cap && used < 24; ++src) {
        unsigned char ch = (unsigned char)*src;
        if (ch >= 32 && ch != 127 && ch != '\t') out[used++] = (char)ch;
    }
    out[used] = 0;
    if (!used) snprintf(out, cap, "Player");
}
static int np_handle_host_response(const char *line)
{
    if (!strncmp(line, "HOSTED\t", 7) && line[7]) {
        np_set_code(line + 7);
        np_set_status(NETPLAY_HOSTING, "Room created; waiting for a player");
        return 1;
    }
    if (!strncmp(line, "ERR\t", 4)) { np_set_status(NETPLAY_ERROR, line + 4); return 0; }
    np_set_status(NETPLAY_ERROR, "Relay sent an unexpected host response");
    return 0;
}
static int np_handle_join_response(const char *line)
{
    if (!strncmp(line, "JOINED\t", 7)) {
        const char *code = line + 7, *tab = strchr(code, '\t');
        char clean_code[16], peer[32];
        size_t len;
        if (!tab) { np_set_status(NETPLAY_ERROR, "Relay sent an invalid join response"); return 0; }
        len = (size_t)(tab - code);
        if (!len || len >= sizeof clean_code) { np_set_status(NETPLAY_ERROR, "Relay sent an invalid invite code"); return 0; }
        memcpy(clean_code, code, len);
        clean_code[len] = 0;
        snprintf(peer, sizeof peer, "%s", tab + 1);
        np_set_code(clean_code);
        np_set_peer(peer);
        np_set_status(NETPLAY_CONNECTED, "Connected to host");
        return 1;
    }
    if (!strncmp(line, "ERR\t", 4)) { np_set_status(NETPLAY_ERROR, line + 4); return 0; }
    np_set_status(NETPLAY_ERROR, "Relay sent an unexpected join response");
    return 0;
}
static int SDLCALL np_worker(void *unused)
{
    char endpoint[160], name[32], join_code[16], host[128], port[8],
         line[NP_NET_MESSAGE_CAP + 2u];
    NetSocket sock;
    int is_host, use_tls;
    (void)unused;
    np_lock();
    snprintf(endpoint, sizeof endpoint, "%s", np_endpoint);
    snprintf(name, sizeof name, "%s", np_name);
    snprintf(join_code, sizeof join_code, "%s", np_join_code);
    is_host = np_is_host;
    np_unlock();

    np_ws_mode = strncmp(endpoint, "wss://", 6) == 0;
    use_tls = np_ws_mode || strncmp(endpoint, "tls://", 6) == 0;
    if (use_tls) memmove(endpoint, endpoint + (np_ws_mode ? 6 : 6),
                         strlen(endpoint + 6) + 1);
    else if (strncmp(endpoint, "tcp://", 6) == 0)
        memmove(endpoint, endpoint + 6, strlen(endpoint + 6) + 1);

    if (!np_parse_endpoint(endpoint, host, sizeof host, port, sizeof port)) {
        np_set_status(NETPLAY_ERROR, "Relay address must be host:port");
        goto finished;
    }
    {
        int blocked_public_target = 0;
        sock = np_connect(host, port, use_tls, &blocked_public_target);
        if (sock == NET_INVALID_SOCKET) {
            np_set_status(NETPLAY_ERROR, blocked_public_target
                ? "Unencrypted relay blocked: use localhost or a private LAN address."
                : (use_tls ? "Couldn't connect to secure relay. Check hostname and port."
                           : "Couldn't connect. Check relay address and port."));
            goto finished;
        }
    }
    np_lock();
    if (np_stop) {
        if (np_socket == sock) np_socket = NET_INVALID_SOCKET;
        np_unlock();
        net_close_socket(sock);
        goto finished;
    }
    np_socket = sock;
    np_unlock();

    if (use_tls) {
        int tls_result = netplay_tls_open(sock, host, np_stopping);
        if (tls_result != 1) {
            if (!np_stopping()) {
                char tls_status[128];
                if (tls_result == -2) {
                    snprintf(tls_status, sizeof tls_status,
                             "TLS CA bundle missing or invalid; reinstall or rebuild PipeClean.");
                } else if (tls_result == -3) {
                    snprintf(tls_status, sizeof tls_status,
                             "TLS certificate rejected. Check the relay hostname and certificate.");
                } else {
                    const char *detail = netplay_tls_last_error();
                    snprintf(tls_status, sizeof tls_status, "Secure TLS failure: %.100s",
                             detail && detail[0] ? detail : "unknown TLS error");
                }
                np_set_status(NETPLAY_ERROR, tls_status);
            }
            goto socket_finished;
        }
    }
    if (np_ws_mode) {
        if (!np_websocket_handshake(sock, host)) {
            if (!np_stopping())
                np_set_status(NETPLAY_ERROR, "WebSocket handshake failed. Check the Render relay URL.");
            goto socket_finished;
        }
        np_ws_ready = 1;
    }

    {
        char safe_name[32], command[160];
        np_safe_name(safe_name, sizeof safe_name, name);
        if (is_host) snprintf(command, sizeof command, "HOST\t%s\n", safe_name);
        else snprintf(command, sizeof command, "JOIN\t%s\t%s\n", join_code, safe_name);
        if (!np_write_transport(sock, command, strlen(command), use_tls)) {
            np_set_status(NETPLAY_ERROR, "Couldn't send session request to relay");
            goto socket_finished;
        }
    }
    {
        int read_ok = np_read_line(sock, line, sizeof line, use_tls);
        if (read_ok <= 0) {
            if (!np_stopping()) np_set_status(NETPLAY_ERROR, "Relay closed before accepting the session");
            goto socket_finished;
        }
        if (!(is_host ? np_handle_host_response(line) : np_handle_join_response(line)))
            goto socket_finished;
    }

    while (!np_stopping()) {
        int read_ok = np_read_line(sock, line, sizeof line, use_tls);
        if (read_ok <= 0) {
            if (!np_stopping()) np_set_status(NETPLAY_ERROR, "Connection to relay was lost");
            break;
        }
        if (!strncmp(line, "PEER_JOINED\t", 12)) {
            int selected;
            char select_line[32];
            np_set_peer(line + 12);
            np_set_status(NETPLAY_CONNECTED, "Player connected to your room");
            np_lock();
            for (int i = 0; i < NETPLAY_SHARED_SLOT_COUNT; i++) {
                np_shared_peer_saved[i] = -1;
                np_shared_local_reported[i] = -1;
                np_shared_name_sent[i] = 0;
                np_shared_name_received[i] = 0;
            }
            np_shared_selection_sent = 0;
            selected = np_selected_game;
            np_unlock();
            if (is_host && (selected == GAME_SML || selected == GAME_SML2)) {
                snprintf(select_line, sizeof select_line, "SELECT_GAME\t%d\n", selected);
                (void)np_send_current(select_line);
            }
        } else if (!strcmp(line, "PEER_LEFT")) {
            np_set_peer("");
            np_lock();
            for (int i = 0; i < NETPLAY_SHARED_SLOT_COUNT; i++) {
                np_shared_peer_saved[i] = -1;
                np_shared_local_reported[i] = -1;
                np_shared_name_sent[i] = 0;
                np_shared_name_received[i] = 0;
            }
            np_shared_selection_sent = 0;
            np_shared_load_slot = -1;
            np_local_rom_valid = np_remote_rom_valid = 0;
            np_local_rom_crc = np_remote_rom_crc = 0;
            np_rom_check_state = NETPLAY_ROM_UNCHECKED;
            np_rom_check_game = np_selected_game;
            snprintf(np_rom_check_status, sizeof np_rom_check_status, "Waiting for a player.");
            np_unlock();
            if (is_host) np_set_status(NETPLAY_HOSTING, "Player left; room is open again");
            else { np_set_status(NETPLAY_ERROR, "Host ended the session"); break; }
        } else if (!strncmp(line, "SELECT_GAME\t", 12)) {
            int selected = atoi(line + 12);
            if (selected == GAME_SML || selected == GAME_SML2) {
                np_lock();
                if (np_selected_game != selected) {
                    for (int i = 0; i < NETPLAY_SHARED_SLOT_COUNT; i++) {
                        np_shared_peer_saved[i] = -1;
                        np_shared_local_reported[i] = -1;
                        np_shared_name_sent[i] = 0;
                        np_shared_name_received[i] = 0;
                    }
                    np_shared_selection_sent = 0;
                    np_shared_load_slot = -1;
                }
                    np_local_rom_valid = np_remote_rom_valid = 0;
                    np_local_rom_crc = np_remote_rom_crc = 0;
                    np_rom_check_state = NETPLAY_ROM_UNCHECKED;
                    np_rom_check_game = selected;
                    snprintf(np_rom_check_status, sizeof np_rom_check_status,
                             "Checking both players' ROMs...");
                }
                np_selected_game = selected;
                np_unlock();
            }
        } else if (!strncmp(line, "START_GAME\t", 11)) {
            int selected = atoi(line + 11);
            np_lock();
            if (selected == np_selected_game &&
                (selected == GAME_SML || selected == GAME_SML2) &&
                np_rom_check_state == NETPLAY_ROM_MATCH) {
                np_start_request = selected;
                if (np_cond) SDL_CondBroadcast(np_cond);
            } else {
                snprintf(np_rom_check_status, sizeof np_rom_check_status,
                         "Host tried to start before both ROMs were verified.");
            }
            np_unlock();
        } else if (!strncmp(line, "START_DENIED\t", 13)) {
            np_lock();
            snprintf(np_rom_check_status, sizeof np_rom_check_status,
                     "Start blocked: %.90s", line + 13);
            np_unlock();
        } else if (!strncmp(line, "CHAT\t", 5)) {
            np_handle_chat_line(line);
        } else if (!strncmp(line, "ROM_RESULT\t", 11)) {
            np_handle_rom_result(line);
        } else if (!strncmp(line, "GAME\t", 5)) {
            np_handle_game_line(line);
        } else if (!strcmp(line, "GAME_END")) {
            np_handle_game_end();
        } else if (!strncmp(line, "INPUT\t", 6)) {
            np_handle_input_line(line);
        } else if (!strncmp(line, "SAVE_NAME\t", 10)) {
            np_handle_save_name(line);
        } else if (!strncmp(line, "SAVE_SELECT\t", 12)) {
            np_handle_save_select(line);
        } else if (!strncmp(line, "SAVE_STATUS\t", 12)) {
            np_handle_save_status(line);
        } else if (!strncmp(line, "SAVE_BEGIN\t", 11)) {
            np_handle_save_begin(line);
        } else if (!strncmp(line, "SAVE_CHUNK\t", 11)) {
            np_handle_save_chunk(line);
        } else if (!strncmp(line, "SAVE_END\t", 9)) {
            np_handle_save_end(line);
        } else if (!strncmp(line, "SAVE_ACK\t", 9)) {
            np_handle_save_ack(line);
        } else if (!strncmp(line, "ERR\t", 4)) {
            np_set_status(NETPLAY_ERROR, line + 4);
            break;
        }
    }

socket_finished:
    if (np_send_mutex) SDL_LockMutex(np_send_mutex);
    np_ws_ready = 0;
    if (use_tls) netplay_tls_close();
    if (np_ws_rng_ready) {
        mbedtls_ctr_drbg_free(&np_ws_drbg);
        mbedtls_entropy_free(&np_ws_entropy);
        np_ws_rng_ready = 0;
    }
    np_lock();
    if (np_socket == sock) np_socket = NET_INVALID_SOCKET;
    np_send_head = np_send_count = 0;
    np_unlock();
    net_shutdown_socket(sock);
    net_close_socket(sock);
    if (np_send_mutex) SDL_UnlockMutex(np_send_mutex);
finished:
    np_lock();
    np_running = 0;
    if (np_cond) SDL_CondBroadcast(np_cond);
    if (np_stop) {
        np_state = NETPLAY_OFFLINE;
        snprintf(np_status, sizeof np_status, "Not connected");
        np_code[0] = np_peer[0] = 0;
    }
    np_unlock();
    return 0;
}
void netplay_init(void)
{
    if (!np_mutex) np_mutex = SDL_CreateMutex();
    if (!np_send_mutex) np_send_mutex = SDL_CreateMutex();
    if (!np_cond) np_cond = SDL_CreateCond();
#ifdef _WIN32
    {
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
            np_set_status(NETPLAY_ERROR, "Winsock initialization failed");
    }
#endif
}
static int netplay_start(int is_host, const char *endpoint, const char *code, const char *name)
{
    SDL_Thread *old = NULL, *created;
    if (!np_mutex) netplay_init();
    np_lock();
    if (np_running) { np_unlock(); return -1; }
    old = np_thread;
    np_thread = NULL;
    np_unlock();
    if (old) SDL_WaitThread(old, NULL);

    np_lock();
    np_stop = 0;
    np_running = 1;
    np_is_host = is_host;
    np_local_game_pending = np_local_game_verified = np_local_game_mismatch = 0;
    np_remote_game_seen = np_game_end_received = 0;
    np_clear_input_queue_locked();
    np_send_head = np_send_count = 0;
    np_tls_mode = endpoint &&
        (strncmp(endpoint, "tls://", 6) == 0 || strncmp(endpoint, "wss://", 6) == 0);
    np_ws_mode = endpoint && strncmp(endpoint, "wss://", 6) == 0;
    np_ws_ready = 0;
    snprintf(np_endpoint, sizeof np_endpoint, "%s", endpoint ? endpoint : "");
    np_safe_name(np_name, sizeof np_name, name);
    snprintf(np_join_code, sizeof np_join_code, "%s", code ? code : "");
    np_code[0] = np_peer[0] = 0;
    np_state = NETPLAY_CONNECTING;
    if (is_host) {
        if (np_selected_game != GAME_SML && np_selected_game != GAME_SML2)
            np_selected_game = GAME_SML2;
    } else {
        np_selected_game = -1;
    }
    np_start_request = -1;
    np_chat_head = np_chat_count = 0;
    np_shared_active_slot = 0;
    np_shared_load_slot = -1;
    np_shared_selection_sent = 0;
    np_shared_tx_active = 0;
    np_shared_tx_slot = -1;
    free(np_shared_rx_data);
    np_shared_rx_data = NULL;
    np_shared_rx_id = np_shared_rx_crc = np_shared_rx_size = np_shared_rx_used = 0;
    np_shared_rx_slot = -1;
    for (int i = 0; i < NETPLAY_SHARED_SLOT_COUNT; i++) {
        np_shared_peer_saved[i] = -1;
        np_shared_local_reported[i] = -1;
        np_shared_name_sent[i] = 0;
        np_shared_name_received[i] = 0;
        np_shared_pending[i] = 0;
    }
    np_rom_check_state = NETPLAY_ROM_UNCHECKED;
    np_rom_check_game = np_selected_game;
    np_local_rom_valid = np_remote_rom_valid = 0;
    np_local_rom_crc = np_remote_rom_crc = 0;
    snprintf(np_rom_check_status, sizeof np_rom_check_status, "Waiting for a player.");
    snprintf(np_status, sizeof np_status, "Connecting to relay...");
    np_unlock();
    created = SDL_CreateThread(np_worker, "PipeCleanNetplay", NULL);
    np_lock();
    np_thread = created;
    if (!created) {
        np_running = 0;
        np_state = NETPLAY_ERROR;
        snprintf(np_status, sizeof np_status, "Couldn't start network worker");
    }
    np_unlock();
    return created ? 0 : -1;
}
int netplay_host(const char *endpoint, const char *name)
{
    return netplay_start(1, endpoint, "", name);
}
int netplay_join(const char *endpoint, const char *code, const char *name)
{
    if (!code || !code[0]) return -1;
    return netplay_start(0, endpoint, code, name);
}
void netplay_disconnect(void)
{
    SDL_Thread *thread;
    NetSocket sock;
    if (!np_mutex) return;
    np_lock();
    np_stop = 1;
    sock = np_socket;
    thread = np_thread;
    np_unlock();
    if (sock != NET_INVALID_SOCKET) net_shutdown_socket(sock);
    if (thread) SDL_WaitThread(thread, NULL);
    np_lock();
    np_thread = NULL;
    np_socket = NET_INVALID_SOCKET;
    np_running = 0;
    np_stop = 0;
    np_state = NETPLAY_OFFLINE;
    snprintf(np_status, sizeof np_status, "Not connected");
    np_code[0] = np_peer[0] = 0;
    np_selected_game = -1;
    np_start_request = -1;
    np_rom_check_state = NETPLAY_ROM_UNCHECKED;
    np_rom_check_game = -1;
    np_local_rom_valid = np_remote_rom_valid = 0;
    np_local_rom_crc = np_remote_rom_crc = 0;
    np_rom_check_status[0] = 0;
    np_chat_head = np_chat_count = 0;
    np_send_head = np_send_count = 0;
    np_tls_mode = 0;
    np_ws_mode = np_ws_ready = 0;
    np_local_game_pending = np_local_game_verified = np_local_game_mismatch = 0;
    np_remote_game_seen = np_game_end_received = 0;
    np_clear_input_queue_locked();
    if (np_cond) SDL_CondBroadcast(np_cond);
    np_unlock();
}
void netplay_get_info(NetplayInfo *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (!np_mutex) {
        out->state = NETPLAY_OFFLINE;
        snprintf(out->status, sizeof out->status, "Not connected");
        return;
    }
    np_lock();
    out->state = np_state;
    out->is_host = np_is_host;
    snprintf(out->status, sizeof out->status, "%s", np_status);
    snprintf(out->code, sizeof out->code, "%s", np_code);
    snprintf(out->peer, sizeof out->peer, "%s", np_peer);
    out->rom_check_state = np_rom_check_state;
    out->rom_check_game = np_rom_check_game;
    snprintf(out->rom_check_status, sizeof out->rom_check_status, "%s", np_rom_check_status);
    np_unlock();
}
void netplay_shutdown(void)
{
    netplay_disconnect();
    if (np_cond) { SDL_DestroyCond(np_cond); np_cond = NULL; }
    if (np_send_mutex) { SDL_DestroyMutex(np_send_mutex); np_send_mutex = NULL; }
    if (np_mutex) {
        SDL_DestroyMutex(np_mutex);
        np_mutex = NULL;
    }
#ifdef _WIN32
    WSACleanup();
#endif
}


int netplay_is_connected(void)
{
    NetplayInfo info;
    netplay_get_info(&info);
    return info.state == NETPLAY_CONNECTED;
}

int netplay_is_host(void)
{
    NetplayInfo info;
    netplay_get_info(&info);
    return info.is_host != 0;
}

int netplay_lobby_set_game(int game)
{
    int should_send;
    char command[32];
    if (game != GAME_SML && game != GAME_SML2) return 0;
    np_lock();
    if (!np_is_host && np_state != NETPLAY_OFFLINE && np_state != NETPLAY_ERROR) {
        np_unlock();
        return 0;
    }
    if (np_selected_game != game) {
        np_rom_check_state = NETPLAY_ROM_UNCHECKED;
        np_rom_check_game = game;
        np_local_rom_valid = np_remote_rom_valid = 0;
        np_local_rom_crc = np_remote_rom_crc = 0;
        snprintf(np_rom_check_status, sizeof np_rom_check_status,
                 "Game changed; checking both ROMs...");
    }
    np_selected_game = game;
    should_send = np_is_host && np_state == NETPLAY_CONNECTED && np_peer[0] != 0;
    np_unlock();
    if (!should_send) return 1;
    snprintf(command, sizeof command, "SELECT_GAME\t%d\n", game);
    return np_send_current(command);
}

int netplay_lobby_get_game(void)
{
    int game;
    np_lock();
    game = np_selected_game;
    np_unlock();
    return game;
}

int netplay_lobby_report_rom(int game, uint32_t rom_crc, int valid)
{
    char command[48];
    int ok_to_send;
    if (game != GAME_SML && game != GAME_SML2) return 0;
    np_lock();
    ok_to_send = np_state == NETPLAY_CONNECTED && np_peer[0] != 0 &&
                  np_selected_game == game && !np_stop;
    if (ok_to_send) {
        np_rom_check_state = NETPLAY_ROM_WAITING;
        np_rom_check_game = game;
        np_local_rom_valid = valid != 0;
        np_local_rom_crc = valid ? rom_crc : 0;
        np_remote_rom_valid = 0;
        np_remote_rom_crc = 0;
        snprintf(np_rom_check_status, sizeof np_rom_check_status,
                 "Waiting for the other player's ROM check...");
    }
    np_unlock();
    if (!ok_to_send) return 0;
    if (valid)
        snprintf(command, sizeof command, "ROM_CHECK\t%d\t%08X\n", game, (unsigned)rom_crc);
    else
        snprintf(command, sizeof command, "ROM_INVALID\t%d\t1\n", game);
    if (!np_send_current(command)) {
        np_lock();
        np_rom_check_state = NETPLAY_ROM_INVALID;
        snprintf(np_rom_check_status, sizeof np_rom_check_status,
                 "Couldn't send the ROM check to the relay.");
        np_unlock();
        return 0;
    }
    return 1;
}

int netplay_lobby_start_game(int game)
{
    char command[32];
    int valid;
    np_lock();
    valid = np_is_host && np_state == NETPLAY_CONNECTED && np_peer[0] != 0 &&
            np_selected_game == game && (game == GAME_SML || game == GAME_SML2) &&
            np_rom_check_state == NETPLAY_ROM_MATCH;
    np_unlock();
    if (!valid) return 0;
    snprintf(command, sizeof command, "START_GAME\t%d\n", game);
    if (!np_send_current(command)) return 0;
    return 1;
}

int netplay_take_start_request(void)
{
    int game;
    np_lock();
    game = np_start_request;
    np_start_request = -1;
    np_unlock();
    return game;
}

int netplay_chat_send(const char *text)
{
    char command[112];
    size_t length = 0;
    if (!text) return 0;
    while (text[length] && length < 88) {
        unsigned char ch = (unsigned char)text[length];
        if (ch < 32 || ch == 127 || ch == '\t' || ch == '\r' || ch == '\n') return 0;
        length++;
    }
    if (!length || text[length] != 0) return 0;
    snprintf(command, sizeof command, "CHAT\t%.*s\n", (int)length, text);
    return np_send_current(command);
}

int netplay_chat_snapshot(NetplayChatMessage *out, int capacity)
{
    int count, start;
    if (!out || capacity <= 0) return 0;
    np_lock();
    count = np_chat_count < capacity ? np_chat_count : capacity;
    start = (np_chat_head + np_chat_count - count) % NP_CHAT_CAP;
    for (int i = 0; i < count; i++)
        out[i] = np_chat[(start + i) % NP_CHAT_CAP];
    np_unlock();
    return count;
}

int netplay_game_begin(int game_id, uint32_t rom_crc)
{
    char message[96];
    Uint32 started;
    int result = -1;
    if (!np_mutex || !np_cond || !netplay_is_connected()) return -1;

    np_lock();
    if (np_stop || !np_running || np_socket == NET_INVALID_SOCKET) {
        np_unlock();
        return -1;
    }
    np_local_game = game_id;
    np_local_crc = rom_crc;
    np_local_game_pending = 1;
    np_local_game_verified = 0;
    np_local_game_mismatch = 0;
    np_game_end_received = 0;
    np_clear_input_queue_locked();
    if (np_remote_game_seen) {
        np_local_game_mismatch = np_remote_game != np_local_game || np_remote_crc != np_local_crc;
        np_local_game_verified = !np_local_game_mismatch;
    }
    np_unlock();

    snprintf(message, sizeof message, "GAME\t%d\t%08X\n", game_id, (unsigned)rom_crc);
    if (!np_send_current(message)) {
        np_lock();
        np_local_game_pending = 0;
        np_unlock();
        return -1;
    }

    started = SDL_GetTicks();
    np_lock();
    while (!np_local_game_verified && !np_local_game_mismatch &&
           !np_game_end_received && np_state == NETPLAY_CONNECTED && np_running) {
        Uint32 elapsed = SDL_GetTicks() - started;
        if (elapsed >= NP_GAME_TIMEOUT_MS) break;
        if (SDL_CondWaitTimeout(np_cond, np_mutex, NP_GAME_TIMEOUT_MS - elapsed) == SDL_MUTEX_TIMEDOUT)
            break;
    }
    if (np_local_game_verified && !np_local_game_mismatch && !np_game_end_received)
        result = 0;
    else {
        np_local_game_pending = 0;
        if (np_local_game_mismatch)
            snprintf(np_status, sizeof np_status, "Game/ROM mismatch. Use identical game files.");
        else if (!np_game_end_received && np_state == NETPLAY_CONNECTED)
            snprintf(np_status, sizeof np_status, "Timed out waiting for the other player to start the same game.");
    }
    np_unlock();
    if (result == 0)
        fprintf(stderr, "[netplay] input delay: %u frames (~%u ms at 60 FPS)\n",
                NP_INPUT_DELAY_FRAMES, (NP_INPUT_DELAY_FRAMES * 1000u + 30u) / 60u);
    return result;
}

int netplay_exchange_inputs(uint64_t frame, uint8_t local_buttons, uint8_t local_dpad, uint8_t local_actions,
                            uint8_t *p1_buttons, uint8_t *p1_dpad, uint8_t *p1_actions,
                            uint8_t *p2_buttons, uint8_t *p2_dpad, uint8_t *p2_actions)
{
    char message[96];
    Uint32 started, wait_ms = 0;
    uint64_t send_frame;
    NetInput remote, delayed_local;
    int got_remote = 0, ok = 0, host;
    if (!p1_buttons || !p1_dpad || !p1_actions || !p2_buttons || !p2_dpad || !p2_actions ||
        !np_mutex || !np_cond) return -1;
    if (frame > UINT64_MAX - NP_INPUT_DELAY_FRAMES) return -1;

    send_frame = frame + NP_INPUT_DELAY_FRAMES;
    np_lock();
    host = np_is_host;
    ok = np_state == NETPLAY_CONNECTED && np_local_game_verified &&
         np_local_game_pending && !np_game_end_received && !np_stop;
    if (ok) {
        NetInput *slot = &np_local_inputs[send_frame % NP_INPUT_CAP];
        slot->frame = send_frame;
        slot->buttons = local_buttons;
        slot->dpad = local_dpad;
        slot->actions = local_actions;
    }
    np_unlock();
    if (!ok) return -1;

    /* Send this physical input early, labelled for the simulation frame that
       will consume it after the fixed delay. Both players delay their own
       inputs too, so both replicas apply the same input pair to every frame. */
    snprintf(message, sizeof message, "INPUT\t%llu\t%u\t%u\t%u\n",
             (unsigned long long)send_frame, (unsigned)local_buttons,
             (unsigned)local_dpad, (unsigned)local_actions);
    if (!np_send_current(message)) return -1;

    started = SDL_GetTicks();
    np_lock();
    memset(&remote, 0, sizeof remote);
    remote.frame = frame;
    for (;;) {
        if (np_game_end_received || np_state != NETPLAY_CONNECTED || np_stop || !np_running)
            break;
        if (np_input_overflow) {
            snprintf(np_status, sizeof np_status, "Online input queue overflowed");
            break;
        }
        /* The first delay-window frames are neutral on both replicas. This
           seeds the pipeline instead of blocking before frame-N input exists. */
        if (frame < NP_INPUT_DELAY_FRAMES) {
            got_remote = 1;
            break;
        }
        while (np_input_count > 0 && np_inputs[np_input_head].frame < frame) {
            np_input_head = (np_input_head + 1) % NP_INPUT_CAP;
            np_input_count--;
        }
        /* Don't rely on queue head matching the current local frame. A future
         * ID at the head used to hide a matching input stored behind it. */
        for (int q = 0; q < np_input_count; q++) {
            int idx = (np_input_head + q) % NP_INPUT_CAP;
            if (np_inputs[idx].frame == frame) {
                remote = np_inputs[idx];
                for (int move = q; move + 1 < np_input_count; move++) {
                    int to = (np_input_head + move) % NP_INPUT_CAP;
                    int from = (np_input_head + move + 1) % NP_INPUT_CAP;
                    np_inputs[to] = np_inputs[from];
                }
                np_input_count--;
                got_remote = 1;
                break;
            }
        }
        if (got_remote) break;
        {
            Uint32 elapsed = SDL_GetTicks() - started;
            if (elapsed >= NP_FRAME_TIMEOUT_MS) {
                snprintf(np_status, sizeof np_status, "Online input timed out; check the connection.");
                break;
            }
            if (SDL_CondWaitTimeout(np_cond, np_mutex, NP_FRAME_TIMEOUT_MS - elapsed) == SDL_MUTEX_TIMEDOUT)
                break;
        }
    }

    if (got_remote && !np_game_end_received && np_state == NETPLAY_CONNECTED) {
        memset(&delayed_local, 0, sizeof delayed_local);
        delayed_local.frame = frame;
        if (frame >= NP_INPUT_DELAY_FRAMES) {
            delayed_local = np_local_inputs[frame % NP_INPUT_CAP];
            if (delayed_local.frame != frame) {
                snprintf(np_status, sizeof np_status,
                         "Online local input buffer lost frame synchronization.");
                np_unlock();
                return -1;
            }
        }

        wait_ms = SDL_GetTicks() - started;
        np_wait_window_total_ms += wait_ms;
        if (wait_ms > np_wait_window_max_ms) np_wait_window_max_ms = wait_ms;
        if (wait_ms >= 8u) np_wait_window_slow++;
        np_wait_window_samples++;
        /* Includes buffered zero-wait frames, so the average shows how much
           wall-clock time the synchronization actually adds per game frame. */
        if (np_wait_window_samples >= 120u) {
            fprintf(stderr, "[netplay] input wait over %u frames: avg=%u ms max=%u ms slow(>=8ms)=%u\n",
                    np_wait_window_samples,
                    np_wait_window_total_ms / np_wait_window_samples,
                    np_wait_window_max_ms, np_wait_window_slow);
            np_wait_window_total_ms = np_wait_window_max_ms = np_wait_window_slow = 0;
            np_wait_window_samples = 0;
        }
        if (host) {
            *p1_buttons = delayed_local.buttons; *p1_dpad = delayed_local.dpad; *p1_actions = delayed_local.actions;
            *p2_buttons = remote.buttons; *p2_dpad = remote.dpad; *p2_actions = remote.actions;
        } else {
            *p1_buttons = remote.buttons; *p1_dpad = remote.dpad; *p1_actions = remote.actions;
            *p2_buttons = delayed_local.buttons; *p2_dpad = delayed_local.dpad; *p2_actions = delayed_local.actions;
        }
        ok = 1;
    } else {
        ok = 0;
    }
    np_unlock();
    return ok ? 0 : -1;
}
static uint32_t np_shared_crc32(const unsigned char *data, size_t length)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1u));
    }
    return ~crc;
}

static int np_shared_write_atomic(const char *path, const void *data, size_t size)
{
    char temp[1240];
    if (!path || !path[0] || !data || !size ||
        snprintf(temp, sizeof temp, "%s.netplay-tmp", path) >= (int)sizeof temp) return 0;
    FILE *file = fopen(temp, "wb");
    if (!file) return 0;
    size_t wrote = fwrite(data, 1, size, file);
    int close_ok = fclose(file) == 0;
    if (wrote != size || !close_ok) { remove(temp); return 0; }
    remove(path);
    if (rename(temp, path) != 0) { remove(temp); return 0; }
    return 1;
}

static int np_shared_write_marker(const char *path)
{
    static const char marker[] = "PipeClean shared save v1\n";
    return np_shared_write_atomic(path, marker, sizeof marker - 1);
}

static int np_shared_parse_u32(const char **cursor, uint32_t *value)
{
    const char *p = *cursor;
    char *end = NULL;
    unsigned long v;
    if (!p || !*p) return 0;
    v = strtoul(p, &end, 10);
    if (end == p || v > UINT32_MAX || (*end != '\t' && *end != '\0')) return 0;
    *value = (uint32_t)v;
    *cursor = *end == '\t' ? end + 1 : end;
    return 1;
}

static void np_handle_save_name(const char *line)
{
    int slot;
    char name[NP_SHARED_NAME_BYTES], extra;
    int parsed = sscanf(line, "SAVE_NAME\t%d\t%31[^\t\r\n]%c", &slot, name, &extra);
    if (parsed < 2 || slot < 0 || slot >= NETPLAY_SHARED_SLOT_COUNT ||
        (parsed == 3 && extra != 0)) return;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++)
        if (*p < 32 || *p == 127) return;
    np_lock();
    if (!np_is_host) {
        snprintf(np_shared_names[slot], sizeof np_shared_names[slot], "%s", name);
        np_shared_name_received[slot] = 1;
    }
    np_unlock();
}

static void np_handle_save_select(const char *line)
{
    int active, load;
    char extra;
    if (sscanf(line, "SAVE_SELECT\t%d\t%d%c", &active, &load, &extra) != 2 ||
        active < 0 || active >= NETPLAY_SHARED_SLOT_COUNT ||
        load < -1 || load >= NETPLAY_SHARED_SLOT_COUNT) return;
    np_lock();
    if (!np_is_host) {
        np_shared_active_slot = active;
        np_shared_load_slot = load;
    }
    np_unlock();
}

static void np_handle_save_status(const char *line)
{
    int slot, saved;
    char extra;
    if (sscanf(line, "SAVE_STATUS\t%d\t%d%c", &slot, &saved, &extra) != 2 ||
        slot < 0 || slot >= NETPLAY_SHARED_SLOT_COUNT || (saved != 0 && saved != 1)) return;
    np_lock();
    np_shared_peer_saved[slot] = saved;
    if (np_is_host && saved == 0 && np_shared_tx_active && np_shared_tx_slot == slot) {
        np_shared_tx_active = 0;
        np_shared_tx_slot = -1;
        np_shared_pending[slot] = 0;
    }
    np_unlock();
}

static void np_handle_save_begin(const char *line)
{
    unsigned id, slot, size, crc;
    char extra;
    if (sscanf(line, "SAVE_BEGIN\t%u\t%u\t%u\t%x%c",
               &id, &slot, &size, &crc, &extra) != 4 ||
        slot >= NETPLAY_SHARED_SLOT_COUNT || size == 0 || size > NP_SHARED_SAVE_MAX_BYTES) return;
    np_lock();
    if (np_is_host || !np_shared_state_path[slot][0] || !np_shared_marker_path[slot][0]) {
        np_unlock();
        return;
    }
    free(np_shared_rx_data);
    np_shared_rx_data = (unsigned char *)malloc(size);
    if (!np_shared_rx_data) {
        np_shared_rx_id = np_shared_rx_crc = np_shared_rx_size = np_shared_rx_used = 0;
        np_shared_rx_slot = -1;
        np_unlock();
        return;
    }
    np_shared_rx_id = id;
    np_shared_rx_slot = (int)slot;
    np_shared_rx_size = size;
    np_shared_rx_crc = crc;
    np_shared_rx_used = 0;
    np_shared_pending[slot] = 1;
    np_shared_local_saved[slot] = 0;
    np_unlock();
}

static void np_handle_save_chunk(const char *line)
{
    const char *cursor = line + 11;
    uint32_t id, slot, offset;
    const char *encoded;
    unsigned char decoded[NP_SHARED_SAVE_CHUNK_RAW];
    size_t decoded_size = 0;
    if (!np_shared_parse_u32(&cursor, &id) ||
        !np_shared_parse_u32(&cursor, &slot) ||
        !np_shared_parse_u32(&cursor, &offset) || !*cursor ||
        slot >= NETPLAY_SHARED_SLOT_COUNT) return;
    encoded = cursor;
    size_t encoded_len = strlen(encoded);
    if (!encoded_len || mbedtls_base64_decode(decoded, sizeof decoded, &decoded_size,
                                               (const unsigned char *)encoded, encoded_len) != 0 ||
        decoded_size == 0 || decoded_size > NP_SHARED_SAVE_CHUNK_RAW) return;
    np_lock();
    if (np_is_host || !np_shared_rx_data || np_shared_rx_id != id ||
        np_shared_rx_slot != (int)slot || offset != np_shared_rx_used ||
        (uint64_t)offset + decoded_size > np_shared_rx_size) {
        np_unlock();
        return;
    }
    memcpy(np_shared_rx_data + offset, decoded, decoded_size);
    np_shared_rx_used += (uint32_t)decoded_size;
    np_unlock();
}

static void np_handle_save_end(const char *line)
{
    unsigned id, slot, size, crc;
    char extra, state_path[1200] = "", marker_path[1200] = "";
    unsigned char *data = NULL;
    int valid = 0;
    if (sscanf(line, "SAVE_END\t%u\t%u\t%u\t%x%c",
               &id, &slot, &size, &crc, &extra) != 4 ||
        slot >= NETPLAY_SHARED_SLOT_COUNT) return;
    np_lock();
    if (!np_is_host && np_shared_rx_data && np_shared_rx_id == id &&
        np_shared_rx_slot == (int)slot && np_shared_rx_size == size &&
        np_shared_rx_used == size && np_shared_rx_crc == crc) {
        data = np_shared_rx_data;
        np_shared_rx_data = NULL;
        snprintf(state_path, sizeof state_path, "%s", np_shared_state_path[slot]);
        snprintf(marker_path, sizeof marker_path, "%s", np_shared_marker_path[slot]);
    }
    np_unlock();
    if (data) {
        valid = np_shared_crc32(data, size) == crc &&
                np_shared_write_atomic(state_path, data, size) &&
                np_shared_write_marker(marker_path);
        free(data);
    }
    np_lock();
    free(np_shared_rx_data);
    np_shared_rx_data = NULL;
    np_shared_rx_id = np_shared_rx_crc = np_shared_rx_size = np_shared_rx_used = 0;
    np_shared_rx_slot = -1;
    np_shared_pending[slot] = 0;
    np_shared_local_saved[slot] = valid;
    np_shared_local_reported[slot] = valid ? 1 : 0;
    np_unlock();
    char command[96];
    snprintf(command, sizeof command, "SAVE_STATUS\t%u\t%d\n", slot, valid ? 1 : 0);
    (void)np_send_current(command);
    if (valid) {
        snprintf(command, sizeof command, "SAVE_ACK\t%u\t%u\t%08X\n", id, slot, crc);
        (void)np_send_current(command);
    }
}

static void np_handle_save_ack(const char *line)
{
    unsigned id, slot, crc;
    char extra, marker_path[1200] = "";
    int accepted = 0;
    if (sscanf(line, "SAVE_ACK\t%u\t%u\t%x%c", &id, &slot, &crc, &extra) != 3 ||
        slot >= NETPLAY_SHARED_SLOT_COUNT) return;
    np_lock();
    if (np_is_host && np_shared_tx_active && np_shared_tx_id == id &&
        np_shared_tx_slot == (int)slot && np_shared_tx_crc == crc) {
        snprintf(marker_path, sizeof marker_path, "%s", np_shared_marker_path[slot]);
        accepted = 1;
    }
    np_unlock();
    if (!accepted) return;
    if (!np_shared_write_marker(marker_path)) {
        np_lock();
        np_shared_tx_active = 0;
        np_shared_tx_slot = -1;
        np_shared_pending[slot] = 0;
        np_shared_local_saved[slot] = 0;
        np_shared_local_reported[slot] = 0;
        np_unlock();
        return;
    }
    np_lock();
    np_shared_tx_active = 0;
    np_shared_tx_slot = -1;
    np_shared_pending[slot] = 0;
    np_shared_local_saved[slot] = 1;
    np_shared_local_reported[slot] = 1;
    np_shared_peer_saved[slot] = 1;
    np_unlock();
    char command[96];
    snprintf(command, sizeof command, "SAVE_STATUS\t%u\t1\n", slot);
    (void)np_send_current(command);
}

void netplay_shared_slot_configure(int slot, const char *state_path,
                                   const char *marker_path, const char *name,
                                   int local_saved)
{
    char state_copy[1200] = "", marker_copy[1200] = "", clean_name[NP_SHARED_NAME_BYTES];
    int send_name = 0, send_status = 0, send_select = 0;
    char command[128];
    if (slot < 0 || slot >= NETPLAY_SHARED_SLOT_COUNT) return;
    snprintf(state_copy, sizeof state_copy, "%s", state_path ? state_path : "");
    snprintf(marker_copy, sizeof marker_copy, "%s", marker_path ? marker_path : "");
    snprintf(clean_name, sizeof clean_name, "%s", name && name[0] ? name : "");
    for (unsigned char *p = (unsigned char *)clean_name; *p; p++)
        if (*p < 32 || *p == 127 || *p == '\t') *p = '_';
    if (!clean_name[0]) snprintf(clean_name, sizeof clean_name, "Slot %d", slot + 1);

    np_lock();
    if (strcmp(np_shared_state_path[slot], state_copy) ||
        strcmp(np_shared_marker_path[slot], marker_copy)) {
        snprintf(np_shared_state_path[slot], sizeof np_shared_state_path[slot], "%s", state_copy);
        snprintf(np_shared_marker_path[slot], sizeof np_shared_marker_path[slot], "%s", marker_copy);
        if (np_is_host || !np_shared_name_received[slot])
            snprintf(np_shared_names[slot], sizeof np_shared_names[slot], "%s", clean_name);
        np_shared_peer_saved[slot] = -1;
        np_shared_local_reported[slot] = -1;
        np_shared_name_sent[slot] = 0;
    }
    np_shared_local_saved[slot] = local_saved != 0;
    if (np_is_host && np_state == NETPLAY_CONNECTED && np_peer[0] &&
        !np_shared_name_sent[slot]) send_name = 1;
    if (np_state == NETPLAY_CONNECTED && np_peer[0] &&
        np_shared_local_reported[slot] != np_shared_local_saved[slot]) send_status = 1;
    if (slot == 0 && np_is_host && np_state == NETPLAY_CONNECTED && np_peer[0] &&
        !np_shared_selection_sent) send_select = 1;
    np_unlock();

    if (send_name) {
        np_lock(); snprintf(clean_name, sizeof clean_name, "%s", np_shared_names[slot]); np_unlock();
        snprintf(command, sizeof command, "SAVE_NAME\t%d\t%s\n", slot, clean_name);
        if (np_send_current(command)) {
            np_lock(); np_shared_name_sent[slot] = 1; np_unlock();
        }
    }
    if (send_status) {
        snprintf(command, sizeof command, "SAVE_STATUS\t%d\t%d\n", slot, local_saved ? 1 : 0);
        if (np_send_current(command)) {
            np_lock(); np_shared_local_reported[slot] = local_saved ? 1 : 0; np_unlock();
        }
    }
    if (send_select) {
        int active, load;
        np_lock(); active=np_shared_active_slot; load=np_shared_load_slot; np_unlock();
        snprintf(command, sizeof command, "SAVE_SELECT\t%d\t%d\n", active, load);
        if (np_send_current(command)) {
            np_lock(); np_shared_selection_sent = 1; np_unlock();
        }
    }
}

void netplay_shared_slot_name(int slot, char *out, size_t out_size)
{
    if (!out || !out_size) return;
    out[0] = 0;
    if (slot < 0 || slot >= NETPLAY_SHARED_SLOT_COUNT) return;
    np_lock();
    snprintf(out, out_size, "%s", np_shared_names[slot][0] ? np_shared_names[slot] : "Slot");
    np_unlock();
}

void netplay_shared_slot_status(int slot, int *local_saved, int *peer_saved, int *pending)
{
    if (local_saved) *local_saved = 0;
    if (peer_saved) *peer_saved = -1;
    if (pending) *pending = 0;
    if (slot < 0 || slot >= NETPLAY_SHARED_SLOT_COUNT) return;
    np_lock();
    if (local_saved) *local_saved = np_shared_local_saved[slot];
    if (peer_saved) *peer_saved = np_shared_peer_saved[slot];
    if (pending) *pending = np_shared_pending[slot];
    np_unlock();
}

int netplay_shared_slot_is_synced(int slot)
{
    int local, peer, pending;
    netplay_shared_slot_status(slot, &local, &peer, &pending);
    return local == 1 && peer == 1 && !pending;
}

int netplay_shared_slot_active(void)
{
    int slot;
    np_lock(); slot = np_shared_active_slot; np_unlock();
    return slot;
}

int netplay_shared_slot_load(void)
{
    int slot;
    np_lock(); slot = np_shared_load_slot; np_unlock();
    return slot;
}

int netplay_shared_slot_select(int active_slot, int load_slot)
{
    char command[64];
    int allowed;
    if (active_slot < 0 || active_slot >= NETPLAY_SHARED_SLOT_COUNT ||
        load_slot < -1 || load_slot >= NETPLAY_SHARED_SLOT_COUNT) return 0;
    np_lock();
    allowed = np_is_host && np_state == NETPLAY_CONNECTED && np_peer[0] &&
              (load_slot < 0 ||
               (np_shared_local_saved[load_slot] == 1 &&
                np_shared_peer_saved[load_slot] == 1 && !np_shared_pending[load_slot]));
    if (allowed) {
        np_shared_active_slot = active_slot;
        np_shared_load_slot = load_slot;
    }
    np_unlock();
    if (!allowed) return 0;
    snprintf(command, sizeof command, "SAVE_SELECT\t%d\t%d\n", active_slot, load_slot);
    if (!np_send_current(command)) return 0;
    np_lock(); np_shared_selection_sent = 1; np_unlock();
    return 1;
}

int netplay_shared_slot_rename(int slot, const char *name)
{
    char command[128], clean_name[NP_SHARED_NAME_BYTES];
    if (slot < 0 || slot >= NETPLAY_SHARED_SLOT_COUNT || !name || !name[0]) return 0;
    snprintf(clean_name, sizeof clean_name, "%s", name);
    for (unsigned char *p = (unsigned char *)clean_name; *p; p++)
        if (*p < 32 || *p == 127 || *p == '\t') return 0;
    np_lock();
    int allowed = np_is_host && np_state == NETPLAY_CONNECTED && np_peer[0];
    if (allowed) snprintf(np_shared_names[slot], sizeof np_shared_names[slot], "%s", clean_name);
    np_unlock();
    if (!allowed) return 0;
    snprintf(command, sizeof command, "SAVE_NAME\t%d\t%s\n", slot, clean_name);
    if (!np_send_current(command)) return 0;
    np_lock();
    np_shared_name_sent[slot] = 1;
    np_unlock();
    return 1;
}

void netplay_shared_slot_mark_unsynced(int slot)
{
    char marker[1200], command[96];
    if (slot < 0 || slot >= NETPLAY_SHARED_SLOT_COUNT) return;
    np_lock();
    snprintf(marker, sizeof marker, "%s", np_shared_marker_path[slot]);
    np_shared_local_saved[slot] = 0;
    np_shared_local_reported[slot] = 0;
    np_shared_pending[slot] = 1;
    np_unlock();
    if (marker[0]) remove(marker);
    snprintf(command, sizeof command, "SAVE_STATUS\t%d\t0\n", slot);
    (void)np_send_current(command);
}

int netplay_shared_save_send_file(int slot, const char *path)
{
    FILE *file;
    unsigned char *data = NULL;
    long file_size;
    uint32_t id, crc;
    char message[NP_NET_MESSAGE_CAP + 1u];
    if (slot < 0 || slot >= NETPLAY_SHARED_SLOT_COUNT || !path || !path[0]) return 0;
    file = fopen(path, "rb");
    if (!file) return 0;
    if (fseek(file, 0, SEEK_END) != 0 || (file_size = ftell(file)) <= 0 ||
        (unsigned long)file_size > NP_SHARED_SAVE_MAX_BYTES ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return 0;
    }
    data = (unsigned char *)malloc((size_t)file_size);
    if (!data) { fclose(file); return 0; }
    if (fread(data, 1, (size_t)file_size, file) != (size_t)file_size) {
        free(data); fclose(file); return 0;
    }
    fclose(file);
    crc = np_shared_crc32(data, (size_t)file_size);
    np_lock();
    if (!np_is_host || np_state != NETPLAY_CONNECTED || !np_peer[0] ||
        np_shared_tx_active || np_shared_state_path[slot][0] == 0) {
        np_unlock(); free(data); return 0;
    }
    id = np_shared_tx_next_id++;
    if (!id) id = np_shared_tx_next_id++;
    np_shared_tx_id = id;
    np_shared_tx_crc = crc;
    np_shared_tx_slot = slot;
    np_shared_tx_active = 1;
    np_shared_pending[slot] = 1;
    np_unlock();

    snprintf(message, sizeof message, "SAVE_BEGIN\t%u\t%u\t%lu\t%08X\n",
             id, (unsigned)slot, (unsigned long)file_size, crc);
    int ok = np_send_current(message);
    for (size_t offset = 0; ok && offset < (size_t)file_size; offset += NP_SHARED_SAVE_CHUNK_RAW) {
        size_t raw = (size_t)file_size - offset;
        if (raw > NP_SHARED_SAVE_CHUNK_RAW) raw = NP_SHARED_SAVE_CHUNK_RAW;
        unsigned char encoded[NP_SHARED_SAVE_CHUNK_RAW * 4u / 3u + 8u];
        size_t encoded_size = 0;
        if (mbedtls_base64_encode(encoded, sizeof encoded, &encoded_size,
                                  data + offset, raw) != 0 || encoded_size + 64 >= sizeof message) {
            ok = 0;
            break;
        }
        encoded[encoded_size] = 0;
        int written = snprintf(message, sizeof message, "SAVE_CHUNK\t%u\t%u\t%lu\t%s\n",
                               id, (unsigned)slot, (unsigned long)offset, encoded);
        if (written < 0 || (size_t)written >= sizeof message) { ok = 0; break; }
        ok = np_send_current(message);
    }
    if (ok) {
        snprintf(message, sizeof message, "SAVE_END\t%u\t%u\t%lu\t%08X\n",
                 id, (unsigned)slot, (unsigned long)file_size, crc);
        ok = np_send_current(message);
    }
    free(data);
    if (!ok) {
        np_lock();
        np_shared_tx_active = 0;
        np_shared_tx_slot = -1;
        np_shared_pending[slot] = 0;
        np_unlock();
    }
    return ok;
}

void netplay_game_end(void)
{
    if (!np_mutex) return;
    if (netplay_is_connected())
        (void)np_send_current("GAME_END\n");
    np_lock();
    np_local_game_pending = 0;
    np_local_game_verified = 0;
    np_local_game_mismatch = 0;
    np_remote_game_seen = 0;
    np_game_end_received = 0;
    np_local_game = np_remote_game = 0;
    np_local_crc = np_remote_crc = 0;
    np_clear_input_queue_locked();
    if (np_state == NETPLAY_CONNECTED)
        snprintf(np_status, sizeof np_status, "Connected; ready to start a game");
    if (np_cond) SDL_CondBroadcast(np_cond);
    np_unlock();
}
