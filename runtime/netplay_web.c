#include "netplay.h"
#include "games.h"

#include <emscripten.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>


static int np_selected_game = GAME_SML2;
static int np_shared_local_saved[NETPLAY_SHARED_SLOT_COUNT];
static char np_shared_names[NETPLAY_SHARED_SLOT_COUNT][32];

EM_JS(void, pc_yg_netplay_init, (), {
    const y = window.__pcYouGame || (window.__pcYouGame = {});
    y.state = 0; y.isHost = false; y.status = "Not connected";
    y.code = ""; y.peer = ""; y.romState = 0; y.romGame = -1;
    y.romStatus = "Waiting for an accepted SML2 ROM.";
    y.selectedGame = 2; y.startRequest = -1; y.playing = false;
    y.localIsP1 = false; y.error = false; y.remoteGame = null;
    y.localRom = null; y.remoteRom = null;
    y.remoteInputs = new Map(); y.localInputs = new Map();
    y.sendInputs = new Map(); y.inputWaiters = new Map();
    y.chat = []; y.room = null; y.baseSha1 = y.baseSha1 || "";
    y.refresh = () => {
        if (!y.room) {
            if (y.state !== 4) y.state = 0;
            return;
        }
        const players = Array.isArray(y.room.players) ? y.room.players : [];
        y.isHost = !!y.room.isHost;
        y.peer = players.filter(p => p.id !== y.room.me).map(p => p.name || "Friend").join(", ");
        y.code = String(y.room.code || "");
        if (y.error) y.state = 4;
        else if (players.length >= 2) y.state = 3;
        else y.state = y.isHost ? 2 : 1;
        if (y.state === 2) y.status = "Private Friends room ready; invite someone.";
        else if (y.state === 3) y.status = "Connected to " + (y.peer || "your friend") + ".";
    };
    if (typeof YouGame !== "undefined" && YouGame.ready) {
        YouGame.ready().then(() => { y.ready = true; }).catch(e => {
            y.state = 4; y.status = "YouGame SDK unavailable: " + String(e && e.message || e);
        });
    }
});

EM_ASYNC_JS(int, pc_yg_join_friends_room, (const char *displayName), {
    const y = window.__pcYouGame || (window.__pcYouGame = {});
    try {
        if (typeof YouGame === "undefined" || !YouGame.multiplayer)
            throw new Error("YouGame SDK did not load.");
        await YouGame.ready();
        if (y.room) return 0;
        if (!y.baseSha1)
            throw new Error("No SML2 base ROM is loaded. Use an accepted base-game copy or the local browser test picker.");
        y.state = 1; y.error = false; y.status = "Opening a private Friends room…";
        const room = await YouGame.multiplayer.joinLobby({
            queue: "friends",
            players: 2,
            minPlayers: 2,
            maxLocalPlayers: 1,
            mode: "pipeclean-coop",
            compatibility: "pipeclean-sml2-v1-" + String(y.baseSha1).toLowerCase()
        });
        y.room = room;
        y.selectedGame = 2;
        y.startRequest = -1;
        y.playing = false;
        y.localRom = null; y.remoteRom = null;
        y.romState = 0; y.romGame = 2;
        y.romStatus = "Waiting for both players to verify their ROM.";
        y.remoteGame = null;
        y.remoteInputs.clear(); y.localInputs.clear(); y.sendInputs.clear(); y.inputWaiters.clear();
        room.on("lobby", () => y.refresh());
        room.on("ready", ({ matchConnections }) => {
            y.localIsP1 = !!(matchConnections && matchConnections[0] === room.me);
            y.playing = true; y.error = false; y.remoteGame = null;
            y.remoteInputs.clear(); y.localInputs.clear();
            y.startRequest = 2;
            y.status = "Match starting; synchronizing Mario and Luigi.";
            y.refresh();
        });
        room.on("message", ({ from, data }) => {
            if (!data || typeof data !== "object") return;
            if (data.t === "pc_rom") {
                y.remoteRom = { game: Number(data.game), crc: Number(data.crc) >>> 0, valid: !!data.valid };
                if (y.localRom && y.remoteRom) {
                    if (!y.localRom.valid || !y.remoteRom.valid || y.localRom.game !== y.remoteRom.game ||
                        y.localRom.crc !== y.remoteRom.crc) {
                        y.romState = 3; y.romStatus = "ROM mismatch: both players need the same SML2 dump.";
                    } else {
                        y.romState = 2; y.romStatus = "Both players have matching SML2 ROMs.";
                    }
                }
            } else if (data.t === "pc_game") {
                y.remoteGame = { game: Number(data.game), crc: Number(data.crc) >>> 0 };
                if (y.gameWaiter) { const fn = y.gameWaiter; y.gameWaiter = null; fn(y.remoteGame); }
            } else if (data.t === "pc_input_batch" && Array.isArray(data.items)) {
                for (const item of data.items) {
                    const frame = Number(item && item.frame);
                    if (!Number.isSafeInteger(frame) || frame < 0) continue;
                    const value = {
                        frame, buttons: Number(item.buttons) & 255,
                        dpad: Number(item.dpad) & 255, actions: Number(item.actions) & 255
                    };
                    y.remoteInputs.set(frame, value);
                    const waiter = y.inputWaiters.get(frame);
                    if (waiter) waiter(value);
                }
            } else if (data.t === "pc_input") {
                const frame = Number(data.frame);
                if (Number.isSafeInteger(frame) && frame >= 0) {
                    const value = {
                        frame, buttons: Number(data.buttons) & 255,
                        dpad: Number(data.dpad) & 255, actions: Number(data.actions) & 255
                    };
                    y.remoteInputs.set(frame, value);
                    const waiter = y.inputWaiters.get(frame);
                    if (waiter) waiter(value);
                }
            } else if (data.t === "pc_chat") {
                y.chat.push({ name: String(data.name || "Friend").slice(0, 31),
                              text: String(data.text || "").slice(0, 95) });
                if (y.chat.length > 8) y.chat.shift();
            }
        });
        room.on("leave", () => {
            y.peer = "";
            if (y.playing) {
                y.error = true; y.state = 4; y.status = "Your friend left the online match.";
                for (const resolve of y.inputWaiters.values()) resolve(null);
                y.inputWaiters.clear();
                if (y.gameWaiter) { y.gameWaiter(null); y.gameWaiter = null; }
            } else y.refresh();
        });
        room.on("result", () => {
            y.playing = false; y.startRequest = -1; y.remoteGame = null; y.refresh();
        });
        room.on("close", ({ reason }) => {
            y.room = null; y.playing = false; y.startRequest = -1;
            y.error = false; y.state = 0; y.code = ""; y.peer = "";
            y.status = "Friends room closed" + (reason ? ": " + reason : "") + ".";
            for (const resolve of y.inputWaiters.values()) resolve(null);
            y.inputWaiters.clear();
            if (y.gameWaiter) { y.gameWaiter(null); y.gameWaiter = null; }
        });
        y.refresh();
        room.showLobby();
        return 1;
    } catch (error) {
        y.room = null; y.state = 4; y.error = true;
        y.status = String(error && error.message || error || "Couldn't join a Friends room.");
        return 0;
    }
});

EM_JS(void, pc_yg_disconnect, (), {
    const y = window.__pcYouGame;
    if (!y) return;
    const room = y.room;
    y.room = null; y.playing = false; y.startRequest = -1;
    y.error = false; y.state = 0; y.code = ""; y.peer = "";
    y.status = "Not connected";
    for (const resolve of y.inputWaiters ? y.inputWaiters.values() : []) resolve(null);
    if (y.inputWaiters) y.inputWaiters.clear();
    if (y.gameWaiter) { y.gameWaiter(null); y.gameWaiter = null; }
    if (room) Promise.resolve(room.leave()).catch(() => {});
});

EM_JS(void, pc_yg_get_info,
      (int *state, int *isHost, char *status, int statusCap, char *code, int codeCap,
       char *peer, int peerCap, int *romState, int *romGame, char *romStatus, int romStatusCap), {
    const y = window.__pcYouGame || {};
    if (typeof y.refresh === "function") y.refresh();
    HEAP32[state >> 2] = Number(y.state || 0);
    HEAP32[isHost >> 2] = y.isHost ? 1 : 0;
    HEAP32[romState >> 2] = Number(y.romState || 0);
    HEAP32[romGame >> 2] = Number(y.romGame === undefined ? -1 : y.romGame);
    stringToUTF8(String(y.status || "Not connected"), status, statusCap);
    stringToUTF8(String(y.code || ""), code, codeCap);
    stringToUTF8(String(y.peer || ""), peer, peerCap);
    stringToUTF8(String(y.romStatus || "Waiting for both ROM checks."), romStatus, romStatusCap);
});

EM_JS(int, pc_yg_start_request, (), {
    const y = window.__pcYouGame || {};
    const request = Number(y.startRequest === undefined ? -1 : y.startRequest);
    y.startRequest = -1;
    return request;
});

EM_ASYNC_JS(int, pc_yg_start_match, (int game), {
    const y = window.__pcYouGame || {};
    try {
        if (!y.room || !y.room.isHost || Number(y.romState) !== 2 || game !== 2) return 0;
        await y.room.beginMatch({ revision: y.room.revision });
        return 1;
    } catch (error) {
        y.status = String(error && error.message || error || "Couldn't start the match.");
        return 0;
    }
});

EM_JS(int, pc_yg_report_rom, (int game, unsigned int crc, int valid), {
    const y = window.__pcYouGame || {};
    if (!y.room || !y.room.opponents || !y.room.opponents.length) return 0;
    y.localRom = { game: Number(game), crc: Number(crc) >>> 0, valid: !!valid };
    y.romGame = Number(game);
    y.romState = valid ? 1 : 4;
    y.romStatus = valid ? "Waiting for the other player's ROM check…" : "The local SML2 ROM is invalid.";
    y.room.send({ t: "pc_rom", game: Number(game), crc: Number(crc) >>> 0, valid: !!valid });
    if (y.remoteRom) {
        if (!valid || !y.remoteRom.valid || y.remoteRom.game !== game || y.remoteRom.crc !== (Number(crc) >>> 0)) {
            y.romState = 3; y.romStatus = "ROM mismatch: both players need the same SML2 dump.";
        } else {
            y.romState = 2; y.romStatus = "Both players have matching SML2 ROMs.";
        }
    }
    return 1;
});

EM_ASYNC_JS(int, pc_yg_game_begin, (int game, unsigned int crc), {
    const y = window.__pcYouGame || {};
    try {
        if (!y.room || !y.room.playing || y.error || game !== 2) return -1;
        y.remoteGame = null;
        const remotePromise = new Promise(resolve => {
            let settled = false;
            const done = value => {
                if (settled) return;
                settled = true; clearTimeout(timer); y.gameWaiter = null; resolve(value);
            };
            const timer = setTimeout(() => done(null), 12000);
            y.gameWaiter = done;
        });
        y.room.send({ t: "pc_game", game, crc: Number(crc) >>> 0 });
        const remote = await remotePromise;
        if (!remote || remote.game !== game || remote.crc !== (Number(crc) >>> 0)) {
            y.error = true; y.state = 4;
            y.status = "Game/ROM mismatch or the other player did not start.";
            return -1;
        }
        return 0;
    } catch (error) {
        y.error = true; y.state = 4; y.status = String(error && error.message || error);
        return -1;
    }
});

EM_ASYNC_JS(int, pc_yg_exchange_inputs,
    (unsigned int frame, unsigned int buttons, unsigned int dpad, unsigned int actions,
     uint8_t *p1b, uint8_t *p1d, uint8_t *p1a, uint8_t *p2b, uint8_t *p2d, uint8_t *p2a), {
    const y = window.__pcYouGame || {};
    try {
        if (!y.room || !y.room.playing || y.error) return -1;
        const delay = 8;
        const sendFrame = frame + delay;
        const localValue = {
            frame: sendFrame, buttons: buttons & 255, dpad: dpad & 255, actions: actions & 255
        };
        y.localInputs.set(sendFrame, localValue);
        y.sendInputs.set(sendFrame, localValue);

        /* The first bundle is frames 8, 9 and 10, sent after frame 10 has
           been sampled. Subsequent bundles repeat every three game frames. */
        if (frame % 3 === 2) {
            const items = [];
            for (let i = sendFrame - 2; i <= sendFrame; i++) {
                const item = y.sendInputs.get(i);
                if (item) items.push(item);
            }
            if (items.length !== 3) {
                y.error = true; y.state = 4; y.status = "Online input packet lost a local frame.";
                return -1;
            }
            y.room.send({ t: "pc_input_batch", items });
            for (const item of items) y.sendInputs.delete(item.frame);
        }

        let remote = null;
        let waitPromise = null;
        if (frame >= delay) {
            if (y.remoteInputs.has(frame)) remote = y.remoteInputs.get(frame);
            else {
                waitPromise = new Promise(resolve => {
                    let settled = false;
                    const done = value => {
                        if (settled) return;
                        settled = true; clearTimeout(timer);
                        y.inputWaiters.delete(frame); resolve(value);
                    };
                    const timer = setTimeout(() => done(null), 10000);
                    y.inputWaiters.set(frame, done);
                });
            }
        }
        if (waitPromise) remote = await waitPromise;
        if (frame >= delay && !remote) {
            y.error = true; y.state = 4; y.status = "Online input timed out; check the connection.";
            return -1;
        }
        const local = frame >= delay ? y.localInputs.get(frame) : { buttons: 0, dpad: 0, actions: 0 };
        if (!local) {
            y.error = true; y.state = 4; y.status = "Online local input buffer lost frame synchronization.";
            return -1;
        }
        const other = remote || { buttons: 0, dpad: 0, actions: 0 };
        if (y.localIsP1) {
            HEAPU8[p1b] = local.buttons; HEAPU8[p1d] = local.dpad; HEAPU8[p1a] = local.actions;
            HEAPU8[p2b] = other.buttons; HEAPU8[p2d] = other.dpad; HEAPU8[p2a] = other.actions;
        } else {
            HEAPU8[p1b] = other.buttons; HEAPU8[p1d] = other.dpad; HEAPU8[p1a] = other.actions;
            HEAPU8[p2b] = local.buttons; HEAPU8[p2d] = local.dpad; HEAPU8[p2a] = local.actions;
        }
        for (const key of y.remoteInputs.keys()) if (key < frame - 16) y.remoteInputs.delete(key);
        for (const key of y.localInputs.keys()) if (key < frame - 16) y.localInputs.delete(key);
        for (const key of y.sendInputs.keys()) if (key < frame - 16) y.sendInputs.delete(key);
        return 0;
    } catch (error) {
        y.error = true; y.state = 4; y.status = String(error && error.message || error);
        return -1;
    }
});

EM_JS(void, pc_yg_game_end, (), {
    const y = window.__pcYouGame || {};
    if (y.room && y.room.playing) y.room.completeMatch({ draw: true }).catch(() => {});
    y.playing = false; y.startRequest = -1; y.remoteGame = null;
    if (y.remoteInputs) y.remoteInputs.clear();
    if (y.localInputs) y.localInputs.clear();
    if (y.sendInputs) y.sendInputs.clear();
    if (y.inputWaiters) { for (const fn of y.inputWaiters.values()) fn(null); y.inputWaiters.clear(); }
    if (typeof y.refresh === "function") y.refresh();
});

EM_JS(int, pc_yg_chat_send, (const char *text), {
    const y = window.__pcYouGame || {};
    if (!y.room) return 0;
    const message = UTF8ToString(text);
    if (!message || message.length > 88 || /[\r\n\t]/.test(message)) return 0;
    const name = String((YouGame.player && YouGame.player.name) || "Player").slice(0, 31);
    y.chat.push({ name, text: message });
    if (y.chat.length > 8) y.chat.shift();
    y.room.send({ t: "pc_chat", name, text: message });
    return 1;
});

EM_JS(int, pc_yg_chat_snapshot, (NetplayChatMessage *out, int capacity), {
    const y = window.__pcYouGame || {};
    const items = Array.isArray(y.chat) ? y.chat.slice(-capacity) : [];
    for (let i = 0; i < items.length; i++) {
        const base = out + i * 128;
        stringToUTF8(String(items[i].name || "Player"), base, 32);
        stringToUTF8(String(items[i].text || ""), base + 32, 96);
    }
    return items.length;
});

EM_JS(void, pc_yg_show_lobby, (), {
    const y = window.__pcYouGame || {};
    if (y.room && typeof y.room.showLobby === "function") y.room.showLobby();
});

void netplay_init(void) { pc_yg_netplay_init(); }
void netplay_shutdown(void) { netplay_disconnect(); }
int netplay_host(const char *endpoint, const char *name)
{
    (void)endpoint;
    return pc_yg_join_friends_room(name ? name : "Player") ? 0 : -1;
}
int netplay_join(const char *endpoint, const char *code, const char *name)
{
    (void)endpoint; (void)code;
    return pc_yg_join_friends_room(name ? name : "Player") ? 0 : -1;
}
void netplay_disconnect(void) { pc_yg_disconnect(); }
void netplay_get_info(NetplayInfo *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    pc_yg_get_info(&out->state, &out->is_host, out->status, sizeof out->status,
                   out->code, sizeof out->code, out->peer, sizeof out->peer,
                   &out->rom_check_state, &out->rom_check_game,
                   out->rom_check_status, sizeof out->rom_check_status);
}
int netplay_is_connected(void)
{
    NetplayInfo info; netplay_get_info(&info);
    return info.state == NETPLAY_CONNECTED;
}
int netplay_is_host(void)
{
    NetplayInfo info; netplay_get_info(&info);
    return info.is_host;
}
void netplay_show_lobby(void) { pc_yg_show_lobby(); }
int netplay_lobby_set_game(int game)
{
    if (game != GAME_SML2) return 0;
    np_selected_game = game;
    return 1;
}
int netplay_lobby_get_game(void) { return np_selected_game; }
int netplay_lobby_start_game(int game) { return pc_yg_start_match(game); }
int netplay_lobby_report_rom(int game, uint32_t crc, int valid)
{
    if (game != GAME_SML2) return 0;
    return pc_yg_report_rom(game, crc, valid);
}
int netplay_take_start_request(void) { return pc_yg_start_request(); }
int netplay_chat_send(const char *text) { return text ? pc_yg_chat_send(text) : 0; }
int netplay_chat_snapshot(NetplayChatMessage *out, int capacity)
{
    return out && capacity > 0 ? pc_yg_chat_snapshot(out, capacity) : 0;
}

void netplay_shared_slot_configure(int slot, const char *state_path,
    const char *marker_path, const char *name, int local_saved)
{
    (void)state_path; (void)marker_path;
    if (slot < 0 || slot >= NETPLAY_SHARED_SLOT_COUNT) return;
    np_shared_local_saved[slot] = local_saved != 0;
    snprintf(np_shared_names[slot], sizeof np_shared_names[slot], "%s",
             name && name[0] ? name : "Slot");
}
void netplay_shared_slot_name(int slot, char *out, size_t out_size)
{
    if (!out || !out_size) return;
    if (slot < 0 || slot >= NETPLAY_SHARED_SLOT_COUNT) { out[0] = 0; return; }
    snprintf(out, out_size, "%s", np_shared_names[slot][0] ? np_shared_names[slot] : "Local slot");
}
void netplay_shared_slot_status(int slot, int *local_saved, int *peer_saved, int *pending)
{
    if (slot < 0 || slot >= NETPLAY_SHARED_SLOT_COUNT) return;
    if (local_saved) *local_saved = np_shared_local_saved[slot];
    if (peer_saved) *peer_saved = 0;
    if (pending) *pending = 0;
}
int netplay_shared_slot_is_synced(int slot) { (void)slot; return 0; }
int netplay_shared_slot_active(void) { return 0; }
int netplay_shared_slot_load(void) { return -1; }
int netplay_shared_slot_select(int active_slot, int load_slot)
{
    (void)active_slot;
    return load_slot < 0;
}
int netplay_shared_slot_rename(int slot, const char *name)
{
    if (slot < 0 || slot >= NETPLAY_SHARED_SLOT_COUNT || !name || !name[0]) return 0;
    snprintf(np_shared_names[slot], sizeof np_shared_names[slot], "%s", name);
    return 1;
}
void netplay_shared_slot_mark_unsynced(int slot) { (void)slot; }
int netplay_shared_save_send_file(int slot, const char *path)
{
    (void)slot; (void)path;
    return 0; /* Account-backed save-file integration is a separate milestone. */
}
int netplay_game_begin(int game, uint32_t crc) { return pc_yg_game_begin(game, crc); }
int netplay_exchange_inputs(uint64_t frame, uint8_t buttons, uint8_t dpad, uint8_t actions,
    uint8_t *p1b, uint8_t *p1d, uint8_t *p1a, uint8_t *p2b, uint8_t *p2d, uint8_t *p2a)
{
    if (frame > 0xFFFFFFFFu) return -1;
    return pc_yg_exchange_inputs((uint32_t)frame, buttons, dpad, actions,
                                 p1b, p1d, p1a, p2b, p2d, p2a);
}
void netplay_game_end(void) { pc_yg_game_end(); }
