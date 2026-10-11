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
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#endif
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "netplay.h"
#include "netplay_tls.h"

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

#define NP_INPUT_CAP 256
#define NP_SEND_CAP 128
#define NP_GAME_TIMEOUT_MS 30000u
#define NP_FRAME_TIMEOUT_MS 15000u
typedef struct { uint64_t frame; uint8_t buttons, dpad, actions; } NetInput;
typedef struct { char data[128]; size_t length; } NetSend;
static NetInput np_inputs[NP_INPUT_CAP];
static int np_input_head, np_input_count, np_input_overflow;
static NetSend np_sends[NP_SEND_CAP];
static int np_send_head, np_send_count;
static int np_tls_mode;
static int np_local_game_pending, np_local_game, np_local_game_verified;
static int np_local_game_mismatch, np_remote_game_seen, np_remote_game;
static uint32_t np_local_crc, np_remote_crc;
static int np_game_end_received;

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
    if (!colon || colon == endpoint || !colon[1]) return 0;
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
static int np_tls_write_all(NetSocket sock, const char *data, size_t length)
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

static int np_write_transport(NetSocket sock, const char *data, size_t length, int use_tls)
{
    return use_tls ? np_tls_write_all(sock, data, length) : np_send_all(sock, data, length);
}

static int np_tls_flush_outgoing(NetSocket sock)
{
    for (;;) {
        char message[128];
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
    char endpoint[160], name[32], join_code[16], host[128], port[8], line[512];
    NetSocket sock;
    int is_host, use_tls;
    (void)unused;
    np_lock();
    snprintf(endpoint, sizeof endpoint, "%s", np_endpoint);
    snprintf(name, sizeof name, "%s", np_name);
    snprintf(join_code, sizeof join_code, "%s", np_join_code);
    is_host = np_is_host;
    np_unlock();

    use_tls = strncmp(endpoint, "tls://", 6) == 0;
    if (use_tls) memmove(endpoint, endpoint + 6, strlen(endpoint + 6) + 1);
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
                np_set_status(NETPLAY_ERROR,
                    tls_result == -2 ? "TLS CA bundle missing or invalid; reinstall or rebuild PipeClean."
                  : tls_result == -3 ? "TLS certificate rejected. Check the relay hostname and certificate."
                                     : "Secure relay TLS handshake failed.");
            }
            goto socket_finished;
        }
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
            np_set_peer(line + 12);
            np_set_status(NETPLAY_CONNECTED, "Player connected to your room");
        } else if (!strcmp(line, "PEER_LEFT")) {
            np_set_peer("");
            if (is_host) np_set_status(NETPLAY_HOSTING, "Player left; room is open again");
            else { np_set_status(NETPLAY_ERROR, "Host ended the session"); break; }
        } else if (!strncmp(line, "GAME\t", 5)) {
            np_handle_game_line(line);
        } else if (!strcmp(line, "GAME_END")) {
            np_handle_game_end();
        } else if (!strncmp(line, "INPUT\t", 6)) {
            np_handle_input_line(line);
        } else if (!strncmp(line, "ERR\t", 4)) {
            np_set_status(NETPLAY_ERROR, line + 4);
            break;
        }
    }

socket_finished:
    if (np_send_mutex) SDL_LockMutex(np_send_mutex);
    if (use_tls) netplay_tls_close();
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
    np_tls_mode = endpoint && strncmp(endpoint, "tls://", 6) == 0;
    snprintf(np_endpoint, sizeof np_endpoint, "%s", endpoint ? endpoint : "");
    np_safe_name(np_name, sizeof np_name, name);
    snprintf(np_join_code, sizeof np_join_code, "%s", code ? code : "");
    np_code[0] = np_peer[0] = 0;
    np_state = NETPLAY_CONNECTING;
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
    np_send_head = np_send_count = 0;
    np_tls_mode = 0;
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
    return result;
}

int netplay_exchange_inputs(uint64_t frame, uint8_t local_buttons, uint8_t local_dpad, uint8_t local_actions,
                            uint8_t *p1_buttons, uint8_t *p1_dpad, uint8_t *p1_actions,
                            uint8_t *p2_buttons, uint8_t *p2_dpad, uint8_t *p2_actions)
{
    char message[96];
    Uint32 started;
    NetInput remote;
    int got_remote = 0, ok = 0, host;
    if (!p1_buttons || !p1_dpad || !p1_actions || !p2_buttons || !p2_dpad || !p2_actions ||
        !np_mutex || !np_cond) return -1;

    np_lock();
    host = np_is_host;
    ok = np_state == NETPLAY_CONNECTED && np_local_game_verified &&
         np_local_game_pending && !np_game_end_received && !np_stop;
    np_unlock();
    if (!ok) return -1;

    snprintf(message, sizeof message, "INPUT\t%llu\t%u\t%u\t%u\n",
             (unsigned long long)frame, (unsigned)local_buttons, (unsigned)local_dpad, (unsigned)local_actions);
    if (!np_send_current(message)) return -1;

    started = SDL_GetTicks();
    np_lock();
    for (;;) {
        if (np_game_end_received || np_state != NETPLAY_CONNECTED || np_stop || !np_running)
            break;
        if (np_input_overflow) {
            snprintf(np_status, sizeof np_status, "Online input queue overflowed");
            break;
        }
        while (np_input_count > 0) {
            NetInput *head = &np_inputs[np_input_head];
            if (head->frame < frame) {
                np_input_head = (np_input_head + 1) % NP_INPUT_CAP;
                np_input_count--;
                continue;
            }
            if (head->frame == frame) {
                remote = *head;
                np_input_head = (np_input_head + 1) % NP_INPUT_CAP;
                np_input_count--;
                got_remote = 1;
            }
            break;
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
        if (host) {
            *p1_buttons = local_buttons; *p1_dpad = local_dpad; *p1_actions = local_actions;
            *p2_buttons = remote.buttons; *p2_dpad = remote.dpad; *p2_actions = remote.actions;
        } else {
            *p1_buttons = remote.buttons; *p1_dpad = remote.dpad; *p1_actions = remote.actions;
            *p2_buttons = local_buttons; *p2_dpad = local_dpad; *p2_actions = local_actions;
        }
        ok = 1;
    } else {
        ok = 0;
    }
    np_unlock();
    return ok ? 0 : -1;
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
