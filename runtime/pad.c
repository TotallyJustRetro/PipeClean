#include <string.h>
#include <math.h>
#include "pad.h"
#include "settings.h"
#include "events.h"
#include "audio.h"

#define MAXPADS 4
typedef struct { SDL_GameController *gc; SDL_Joystick *joy; int mapped; } Pad;
static Pad pads[MAXPADS];
static int n_pads;
static const char *name_of(const Pad *p) { const char *n = p->mapped ? SDL_GameControllerName(p->gc) : SDL_JoystickName(p->joy); return n && n[0] ? n : "Controller"; }
static void rescan(void)
{
    for (int i = 0; i < n_pads; i++) { if (pads[i].gc) SDL_GameControllerClose(pads[i].gc); else if (pads[i].joy) SDL_JoystickClose(pads[i].joy); }
    memset(pads, 0, sizeof pads); n_pads = 0;
    int nj = SDL_NumJoysticks();
    for (int i = 0; i < nj && n_pads < MAXPADS; i++) {
        if (SDL_IsGameController(i)) { SDL_GameController *c = SDL_GameControllerOpen(i); if (c) { pads[n_pads].gc = c; pads[n_pads].mapped = 1; n_pads++; } }
        else { SDL_Joystick *j = SDL_JoystickOpen(i); if (j) { pads[n_pads].joy = j; n_pads++; } }
    }
}

void pad_init(void)
{
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4_RUMBLE, "1");
    SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK);
    rescan();
}

void pad_shutdown(void)
{
    for (int i = 0; i < n_pads; i++) { if (pads[i].gc) SDL_GameControllerClose(pads[i].gc); else if (pads[i].joy) SDL_JoystickClose(pads[i].joy); }
    n_pads = 0;
    audio_pad_close();
}

void pad_event(const SDL_Event *e)
{
    if (e->type == SDL_CONTROLLERDEVICEADDED || e->type == SDL_CONTROLLERDEVICEREMOVED || e->type == SDL_JOYDEVICEADDED || e->type == SDL_JOYDEVICEREMOVED) {
        rescan();
        if (e->type == SDL_CONTROLLERDEVICEADDED && pad_is_dualsense(0) + pad_is_dualsense(1) > 0) audio_pad_open();
    }
}

int pad_count(void) { return n_pads; }
const char *pad_name(int i) { return i >= 0 && i < n_pads ? name_of(&pads[i]) : ""; }
int pad_is_dualsense(int i)
{
    if (i < 0 || i >= n_pads) return 0;
    return pads[i].mapped && SDL_GameControllerGetType(pads[i].gc) == SDL_CONTROLLER_TYPE_PS5;
}
int pad_is_edge(int i)
{
    if (!pad_is_dualsense(i)) return 0;
    return SDL_GameControllerGetVendor(pads[i].gc) == 0x054C && SDL_GameControllerGetProduct(pads[i].gc) == 0x0DF2;
}

int pad_is_dualsense_instance(SDL_JoystickID which)
{
    for (int i = 0; i < n_pads; i++) {
        SDL_Joystick *j = pads[i].mapped ? SDL_GameControllerGetJoystick(pads[i].gc) : pads[i].joy;
        if (pad_is_dualsense(i) && j && SDL_JoystickInstanceID(j) == which) return 1;
    }
    return 0;
}

int pad_touchpad_event(const SDL_Event *e, float *x, float *y, int *kind)
{
    if (!e) return 0;
    int k = -1;
    if (e->type == SDL_CONTROLLERTOUCHPADDOWN) k = 0;
    else if (e->type == SDL_CONTROLLERTOUCHPADMOTION) k = 1;
    else if (e->type == SDL_CONTROLLERTOUCHPADUP) k = 2;
    else return 0;
    if (!pad_is_dualsense_instance(e->ctouchpad.which)) return 0;
    if (x) *x = e->ctouchpad.x;
    if (y) *y = e->ctouchpad.y;
    if (kind) *kind = k;
    return 1;
}

const char *pad_status(char *buf, size_t n)
{
    if (!n_pads) { snprintf(buf, n, "No controller connected"); return buf; }
    int i = 0;
    for (int k = 0; k < n_pads; k++) if (pad_is_dualsense(k)) { i = k; break; }
    const char *kind = pad_is_edge(i) ? "DualSense Edge" : (pad_is_dualsense(i) ? "DualSense" : pad_name(i));
    const char *conn = "";
    SDL_JoystickPowerLevel pl = SDL_JoystickCurrentPowerLevel(pads[i].mapped ? SDL_GameControllerGetJoystick(pads[i].gc) : pads[i].joy);
    conn = pl == SDL_JOYSTICK_POWER_WIRED ? " (USB)" : (pl == SDL_JOYSTICK_POWER_UNKNOWN ? "" : " (wireless)");
    snprintf(buf, n, "%s connected%s%s", kind, conn, n_pads > 1 ? " + more" : "");
    return buf;
}

/* ---------------------------------------------------------------- input */
static int pad_down(int device, int code)
{
    if (device < 0 || device >= n_pads) return 0;
    Pad *p = &pads[device];
    if (p->mapped) {
        if (code >= PAD_AXIS_BASE) return SDL_GameControllerGetAxis(p->gc, code == PAD_AXIS_BASE ? SDL_CONTROLLER_AXIS_TRIGGERLEFT : SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 12000;
        return code >= 0 && SDL_GameControllerGetButton(p->gc, (SDL_GameControllerButton)code) != 0;
    }
    if (code >= PAD_AXIS_BASE) { int axis = code == PAD_AXIS_BASE ? 4 : 5; return axis < SDL_JoystickNumAxes(p->joy) && SDL_JoystickGetAxis(p->joy, axis) > 12000; }
    return code >= 0 && code < SDL_JoystickNumButtons(p->joy) && SDL_JoystickGetButton(p->joy, code) != 0;
}

static void pad_poll_one(const GameCfg *c, int player, uint8_t *b, uint8_t *d)
{
    const Uint8 *ks = SDL_GetKeyboardState(NULL);
    uint8_t bits[N_BTN] = {0};
    if (player < 0 || player > 1) player = 0;
    for (int i = 0; i < N_BTN; i++) {
        if (c->key[i][player]) {
            SDL_Scancode sc = SDL_GetScancodeFromKey(c->key[i][player]);
            if (sc != SDL_SCANCODE_UNKNOWN && ks[sc]) bits[i] = 1;
        }
        if (c->pad[i][player] >= 0 && pad_down(c->pad_device[player], c->pad[i][player])) bits[i] = 1;
    }

    /*
     * SML1 multiplayer always keeps a guaranteed Player 2 keyboard fallback.
     * This also repairs older INI files that predate the second-player bindings.
     */
    if (player == 1 && c->multiplayer) {
        if (ks[SDL_SCANCODE_J]) bits[BTN_A] = 1;
        if (ks[SDL_SCANCODE_K]) bits[BTN_B] = 1;
        if (ks[SDL_SCANCODE_D]) bits[BTN_RIGHT] = 1;
        if (ks[SDL_SCANCODE_A]) bits[BTN_LEFT] = 1;
        if (ks[SDL_SCANCODE_W]) bits[BTN_UP] = 1;
        if (ks[SDL_SCANCODE_S]) bits[BTN_DOWN] = 1;
    }
    float dz = settings.pad_deadzone / 100.0f * 32767.0f;
    int device = c->pad_device[player];
    if (device >= 0 && device < n_pads) {
        int ax = pads[device].mapped ? SDL_GameControllerGetAxis(pads[device].gc, SDL_CONTROLLER_AXIS_LEFTX) :
                 (SDL_JoystickNumAxes(pads[device].joy) > 0 ? SDL_JoystickGetAxis(pads[device].joy, 0) : 0);
        int ay = pads[device].mapped ? SDL_GameControllerGetAxis(pads[device].gc, SDL_CONTROLLER_AXIS_LEFTY) :
                 (SDL_JoystickNumAxes(pads[device].joy) > 1 ? SDL_JoystickGetAxis(pads[device].joy, 1) : 0);
        if (ax > dz) bits[BTN_RIGHT] = 1; else if (ax < -dz) bits[BTN_LEFT] = 1;
        if (ay > dz) bits[BTN_DOWN] = 1; else if (ay < -dz) bits[BTN_UP] = 1;
        if (!pads[device].mapped && SDL_JoystickNumHats(pads[device].joy) > 0) {
            Uint8 hat = SDL_JoystickGetHat(pads[device].joy, 0);
            if (hat & SDL_HAT_RIGHT) bits[BTN_RIGHT] = 1;
            if (hat & SDL_HAT_LEFT) bits[BTN_LEFT] = 1;
            if (hat & SDL_HAT_UP) bits[BTN_UP] = 1;
            if (hat & SDL_HAT_DOWN) bits[BTN_DOWN] = 1;
        }
    }
    *b = (uint8_t)(bits[BTN_A] | bits[BTN_B] << 1 | bits[BTN_SELECT] << 2 | bits[BTN_START] << 3);
    *d = (uint8_t)(bits[BTN_RIGHT] | bits[BTN_LEFT] << 1 | bits[BTN_UP] << 2 | bits[BTN_DOWN] << 3);
}

void pad_poll_player(int game, int player, uint8_t *b, uint8_t *d)
{
    if (!b || !d || game < 0) return;
    pad_poll_one(&settings.g[game], player, b, d);
    if ((*d & 3) == 3) *d &= (uint8_t)~3;
    if ((*d & 12) == 12) *d &= (uint8_t)~12;
}

void pad_poll(int game, uint8_t *b, uint8_t *d)
{
    uint8_t b0 = 0, d0 = 0, b1 = 0, d1 = 0;
    pad_poll_player(game, 0, &b0, &d0);
    pad_poll_player(game, 1, &b1, &d1);
    *b = (uint8_t)(b0 | b1);
    *d = (uint8_t)(d0 | d1);
    if ((*d & 3) == 3) *d &= (uint8_t)~3;
    if ((*d & 12) == 12) *d &= (uint8_t)~12;
}


static int pad_instance_device(SDL_JoystickID which);

int pad_is_selected_dualsense_instance(int game, SDL_JoystickID which)
{
    if (game < 0 || game >= N_GAMES) return 0;
    int device = settings.g[game].pad_device[0];
    return device >= 0 && device < n_pads && pad_is_dualsense(device) && pad_instance_device(which) == device;
}

int pad_binding_down(int game, int action)
{
    if (game < 0 || game >= N_GAMES || action < 0 || action >= N_ACTION) return 0;
    int code = settings.g[game].action_pad[action];
    if (code < 0) return 0;
    int device = settings.g[game].pad_device[0];
    return device >= 0 && device < n_pads && pad_down(device, code);
}

int pad_binding_event(int game, int action, const SDL_Event *e)
{
    if (game < 0 || game >= N_GAMES || action < 0 || action >= N_ACTION || !e) return 0;
    int device = settings.g[game].pad_device[0];
    if (device < 0 || device >= n_pads) return 0;
    SDL_JoystickID which = -1;
    if (e->type == SDL_CONTROLLERBUTTONDOWN) which = e->cbutton.which;
    else if (e->type == SDL_CONTROLLERAXISMOTION) which = e->caxis.which;
    else if (e->type == SDL_JOYBUTTONDOWN) which = e->jbutton.which;
    else if (e->type == SDL_JOYAXISMOTION) which = e->jaxis.which;
    if (pad_instance_device(which) != device) return 0;
    int code = settings.g[game].action_pad[action];
    if (code < 0) return 0;
    if (e->type == SDL_CONTROLLERBUTTONDOWN) return e->cbutton.button == code;
    if (e->type == SDL_JOYBUTTONDOWN) return e->jbutton.button == code;
    if (e->type == SDL_CONTROLLERAXISMOTION && e->caxis.value > 20000) {
        if (code == PAD_AXIS_BASE && e->caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT) return 1;
        if (code == PAD_AXIS_BASE + 1 && e->caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) return 1;
    }
    if (e->type == SDL_JOYAXISMOTION && e->jaxis.value > 20000) {
        if (code == PAD_AXIS_BASE && e->jaxis.axis == 4) return 1;
        if (code == PAD_AXIS_BASE + 1 && e->jaxis.axis == 5) return 1;
    }
    return 0;
}

static int pad_instance_device(SDL_JoystickID which)
{
    for (int i = 0; i < n_pads; i++) {
        SDL_Joystick *j = pads[i].mapped ? SDL_GameControllerGetJoystick(pads[i].gc) : pads[i].joy;
        if (j && SDL_JoystickInstanceID(j) == which) return i;
    }
    return -1;
}

int pad_capture(int device, const SDL_Event *e)
{
    if (!e) return -1;
    if (device >= 0) {
        SDL_JoystickID which = -1;
        if (e->type == SDL_CONTROLLERBUTTONDOWN) which = e->cbutton.which;
        else if (e->type == SDL_CONTROLLERAXISMOTION) which = e->caxis.which;
        else if (e->type == SDL_JOYBUTTONDOWN) which = e->jbutton.which;
        else if (e->type == SDL_JOYAXISMOTION) which = e->jaxis.which;
        if (pad_instance_device(which) != device) return -1;
    }
    if (e->type == SDL_CONTROLLERBUTTONDOWN) return e->cbutton.button;
    if (e->type == SDL_JOYBUTTONDOWN) return e->jbutton.button;
    if (e->type == SDL_JOYAXISMOTION && e->jaxis.value > 20000) {
        if (e->jaxis.axis == 4) return PAD_AXIS_BASE;
        if (e->jaxis.axis == 5) return PAD_AXIS_BASE + 1;
    }
    if (e->type == SDL_CONTROLLERAXISMOTION && e->caxis.value > 20000) {
        if (e->caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT) return PAD_AXIS_BASE;
        if (e->caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) return PAD_AXIS_BASE + 1;
    }
    return -1;
}

const char *pad_code_name_device(int device, int code, char *buf, size_t n)
{
    if (code < 0) { snprintf(buf, n, "-"); return buf; }
    int ps = pad_is_dualsense(device);
    if (code == PAD_AXIS_BASE) { snprintf(buf, n, ps ? "L2" : "LT"); return buf; }
    if (code == PAD_AXIS_BASE + 1) { snprintf(buf, n, ps ? "R2" : "RT"); return buf; }
    static const char *xb[] = {"A", "B", "X", "Y", "Back", "Guide", "Start", "L3", "R3", "LB", "RB", "Up", "Down", "Left", "Right", "Share", "Paddle 1", "Paddle 2", "Paddle 3", "Paddle 4", "Touchpad"};
    static const char *dsn[] = {"Cross", "Circle", "Square", "Triangle", "Create", "PS", "Options", "L3", "R3", "L1", "R1", "Up", "Down", "Left", "Right", "Mute", "Right paddle", "Left paddle", "Right Fn", "Left Fn", "Touchpad"};
    if (code < (int)(sizeof xb / sizeof xb[0])) snprintf(buf, n, "%s", ps ? dsn[code] : xb[code]);
    else snprintf(buf, n, "Button %d", code);
    return buf;
}

const char *pad_code_name(int code, char *buf, size_t n)
{
    return pad_code_name_device(n_pads > 0 ? 0 : -1, code, buf, n);
}

const char *key_code_name(int key, char *buf, size_t n)
{
    if (!key) { snprintf(buf, n, "-"); return buf; }
    const char *k = SDL_GetKeyName(key);
    snprintf(buf, n, "%s", k && k[0] ? k : "?");
    return buf;
}

/* ---------------------------------------------------------------- light + rumble */
static int ctx_game = -1, ctx_in_game;
static uint32_t screen_rgb = 0x808080;
static uint32_t flash_rgb;
static float flash_t;
static uint32_t last_sent = 0xFFFFFFFFu;
static float send_acc;

void pad_set_context(int game, int in_game) { ctx_game = game; ctx_in_game = in_game; last_sent = 0xFFFFFFFFu; }
void pad_set_screen_color(uint32_t rgb) { screen_rgb = rgb; }
void pad_flash(uint32_t rgb) { flash_rgb = rgb; flash_t = 1.0f; }

int pad_has_light(void)
{
    for (int i = 0; i < n_pads; i++) if (pads[i].mapped && SDL_GameControllerHasLED(pads[i].gc)) return 1;
    return 0;
}

void pad_rumble(int pct, int ms)
{
    if (pct <= 0) return;
    if (pct > 100) pct = 100;
    Uint16 lo = (Uint16)(pct * 655), hi = (Uint16)(pct * 655 * 0.8);
    for (int i = 0; i < n_pads; i++) if (pads[i].mapped && SDL_GameControllerHasRumble(pads[i].gc)) SDL_GameControllerRumble(pads[i].gc, lo, hi, (Uint32)ms);
}

void pad_event_fx(int game, int ev)
{
    const EventDef *d = events_def(game, ev);
    if (!d) return;
    const GameCfg *c = &settings.g[game];
    if ((c->ds_ev_rumble >> ev) & 1) pad_rumble(c->ds_rumble * d->rumble_str / 100, d->rumble_ms);
    if ((c->ds_ev_led >> ev) & 1) pad_flash(d->color);
}

static uint32_t base_color(const GameCfg *c)
{
    switch (c->ds_led_mode) {
    case LED_PALETTE: return palettes[c->palette].bg[1];
    case LED_CUSTOM: return c->ds_color;
    case LED_SCREEN: return screen_rgb;
    default: return 0;
    }
}

void pad_frame(float dt)
{
    if (ctx_game < 0 || !n_pads) return;
    const GameCfg *c = &settings.g[ctx_game];
    uint32_t base = base_color(c);
    float k = c->ds_bright / 100.0f;
    float r = ((base >> 16) & 255) * k, g = ((base >> 8) & 255) * k, b = (base & 255) * k;
    if (flash_t > 0) {
        float f = flash_t * flash_t;
        r += (((flash_rgb >> 16) & 255) - r) * f; g += (((flash_rgb >> 8) & 255) - g) * f; b += ((flash_rgb & 255) - b) * f;
        flash_t -= dt / 0.40f;
        if (flash_t < 0) flash_t = 0;
    }
    uint32_t out = ((uint32_t)(r + 0.5f) << 16) | ((uint32_t)(g + 0.5f) << 8) | (uint32_t)(b + 0.5f);
    send_acc += dt;
    if (out == last_sent || send_acc < 0.033f) return;
    send_acc = 0;
    last_sent = out;
    for (int i = 0; i < n_pads; i++) if (pads[i].mapped && SDL_GameControllerHasLED(pads[i].gc)) SDL_GameControllerSetLED(pads[i].gc, (Uint8)(out >> 16), (Uint8)(out >> 8), (Uint8)out);
}