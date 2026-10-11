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
#include <unistd.h>
#include <errno.h>
#endif
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "netplay.h"

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
static SDL_Thread *np_thread;
static NetSocket np_socket = NET_INVALID_SOCKET;
static int np_state = NETPLAY_OFFLINE;
static int np_running, np_stop, np_is_host;
static char np_status[128] = "Not connected";
static char np_code[16], np_peer[32];
static char np_endpoint[160], np_name[32], np_join_code[16];

static void np_lock(void) { if (np_mutex) SDL_LockMutex(np_mutex); }
static void np_unlock(void) { if (np_mutex) SDL_UnlockMutex(np_mutex); }

static void np_set_status(int state, const char *status)
{
    np_lock();
    np_state = state;
    snprintf(np_status, sizeof np_status, "%s", status ? status : "");
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

static NetSocket np_connect(const char *host, const char *port)
{
    struct addrinfo hints, *list = NULL, *it;
    NetSocket sock = NET_INVALID_SOCKET;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    if (getaddrinfo(host, port, &hints, &list) != 0) return NET_INVALID_SOCKET;
    for (it = list; it; it = it->ai_next) {
        sock = (NetSocket)socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (sock == NET_INVALID_SOCKET) continue;
        if (connect(sock, it->ai_addr, (int)it->ai_addrlen) == 0) break;
        net_close_socket(sock);
        sock = NET_INVALID_SOCKET;
    }
    freeaddrinfo(list);
    return sock;
}
static int np_send_all(NetSocket sock, const char *data, size_t length)
{
    size_t sent = 0;
    while (sent < length) {
#ifdef _WIN32
        int n = send(sock, data + sent, (int)(length - sent), 0);
#else
        ssize_t n = send(sock, data + sent, length - sent, 0);
#endif
        if (n <= 0) return 0;
        sent += (size_t)n;
    }
    return 1;
}
static int np_read_line(NetSocket sock, char *out, size_t cap)
{
    size_t used = 0;
    if (!out || cap < 2) return 0;
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
    int is_host;
    (void)unused;
    np_lock();
    snprintf(endpoint, sizeof endpoint, "%s", np_endpoint);
    snprintf(name, sizeof name, "%s", np_name);
    snprintf(join_code, sizeof join_code, "%s", np_join_code);
    is_host = np_is_host;
    np_unlock();

    if (!np_parse_endpoint(endpoint, host, sizeof host, port, sizeof port)) {
        np_set_status(NETPLAY_ERROR, "Relay address must be host:port");
        goto finished;
    }
    sock = np_connect(host, port);
    if (sock == NET_INVALID_SOCKET) {
        np_set_status(NETPLAY_ERROR, "Couldn't connect. Check relay address and port.");
        goto finished;
    }
    np_lock();
    if (np_stop) {
        np_unlock();
        net_close_socket(sock);
        goto finished;
    }
    np_socket = sock;
    np_unlock();

    {
        char safe_name[32], command[160];
        np_safe_name(safe_name, sizeof safe_name, name);
        if (is_host) snprintf(command, sizeof command, "HOST\t%s\n", safe_name);
        else snprintf(command, sizeof command, "JOIN\t%s\t%s\n", join_code, safe_name);
        if (!np_send_all(sock, command, strlen(command))) {
            np_set_status(NETPLAY_ERROR, "Couldn't send session request to relay");
            goto socket_finished;
        }
    }
    {
        int read_ok = np_read_line(sock, line, sizeof line);
        if (read_ok <= 0) {
            if (!np_stopping()) np_set_status(NETPLAY_ERROR, "Relay closed before accepting the session");
            goto socket_finished;
        }
        if (!(is_host ? np_handle_host_response(line) : np_handle_join_response(line)))
            goto socket_finished;
    }

    while (!np_stopping()) {
        int read_ok = np_read_line(sock, line, sizeof line);
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
        } else if (!strncmp(line, "ERR\t", 4)) {
            np_set_status(NETPLAY_ERROR, line + 4);
            break;
        }
        /* Reserved for input/state packets in the gameplay-sync phase. */
    }

socket_finished:
    net_shutdown_socket(sock);
    net_close_socket(sock);
    np_lock();
    if (np_socket == sock) np_socket = NET_INVALID_SOCKET;
    np_unlock();
finished:
    np_lock();
    np_running = 0;
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
    snprintf(out->status, sizeof out->status, "%s", np_status);
    snprintf(out->code, sizeof out->code, "%s", np_code);
    snprintf(out->peer, sizeof out->peer, "%s", np_peer);
    np_unlock();
}
void netplay_shutdown(void)
{
    netplay_disconnect();
    if (np_mutex) {
        SDL_DestroyMutex(np_mutex);
        np_mutex = NULL;
    }
#ifdef _WIN32
    WSACleanup();
#endif
}
