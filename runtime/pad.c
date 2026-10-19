#include <string.h>
#include <math.h>
#ifdef __ANDROID__
#include <SDL_haptic.h>
#include <SDL_system.h>
#include <jni.h>
#endif
#include "pad.h"
#include "settings.h"
#include "events.h"
#include "audio.h"

#define MAXPADS 4
typedef struct {
    SDL_GameController *gc;
    SDL_Joystick *joy;
    int mapped;
#ifdef __ANDROID__
    SDL_Haptic *haptic; /* Android maps this to InputDevice.getVibrator(). */
#endif
} Pad;
static Pad pads[MAXPADS];
static int n_pads;
static SDL_JoystickID assigned_instance[N_GAMES][MAX_MP_PLAYERS];
static int sync_initialized;
#ifdef __ANDROID__
/* SDL2's Android joystick rumble backend is unsupported. Its separate haptic
 * backend exposes controller vibrators and a VIBRATOR_SERVICE device. */
static SDL_Haptic *android_system_haptic;
#endif
static uint8_t touch_buttons[N_GAMES][MAX_MP_PLAYERS];
static uint8_t touch_dpad[N_GAMES][MAX_MP_PLAYERS];
static const char *name_of(const Pad *p) { const char *n = p->mapped ? SDL_GameControllerName(p->gc) : SDL_JoystickName(p->joy); return n && n[0] ? n : "Controller"; }
static int pad_instance_device(SDL_JoystickID which);
static SDL_Joystick *joystick_for_device(int device)
{
    if (device < 0 || device >= n_pads) return NULL;
    return pads[device].mapped ? SDL_GameControllerGetJoystick(pads[device].gc) : pads[device].joy;
}
static SDL_JoystickID device_instance(int device)
{
    SDL_Joystick *j = joystick_for_device(device);
    return j ? SDL_JoystickInstanceID(j) : (SDL_JoystickID)-1;
}
static int pad_find_instance(SDL_JoystickID which)
{
    if (which < 0) return -1;
    for (int i = 0; i < n_pads; i++) if (device_instance(i) == which) return i;
    return -1;
}
static int pad_device_raw_guid(int device, char *buf, size_t n)
{
    SDL_Joystick *j = joystick_for_device(device);
    if (!buf || !n) return 0;
    buf[0] = 0;
    if (!j) return 0;
    SDL_JoystickGUID guid = SDL_JoystickGetGUID(j);
    SDL_JoystickGetGUIDString(guid, buf, (int)n);
    return buf[0] != 0;
}
static int identity_is_strong(const char *s)
{
    return s && (s[0] == 'S' || s[0] == 'P') && s[1] == ':';
}
/* Old configs contain a bare 32-character GUID. New fallback identities start G:<guid>|<name>. */
static int identity_weak_guid(const char *s, char out[33])
{
    if (!s || !s[0] || !out) return 0;
    if (strlen(s) == 32) {
        memcpy(out, s, 32); out[32] = 0;
        return 1;
    }
    if (s[0] == 'G' && s[1] == ':' && strlen(s) >= 35 && s[34] == '|') {
        memcpy(out, s + 2, 32); out[32] = 0;
        return 1;
    }
    return 0;
}
static int identity_matches_device(const char *identity, int device)
{
    if (!identity || !identity[0]) return 0;
    if (identity_is_strong(identity)) {
        char current[512];
        return pad_device_identity(device, current, sizeof current) && !strcmp(identity, current);
    }
    char want[33], got[40];
    if (!identity_weak_guid(identity, want) || !pad_device_raw_guid(device, got, sizeof got)) return 0;
    return !strcmp(want, got);
}
static int weak_guid_candidate_count(const char *guid, const int claimed[MAXPADS])
{
    int count = 0;
    char got[40];
    for (int i = 0; i < n_pads; i++)
        if ((!claimed || !claimed[i]) && pad_device_raw_guid(i, got, sizeof got) && !strcmp(guid, got)) count++;
    return count;
}
static int unique_unclaimed_identity(const char *identity, const int claimed[MAXPADS])
{
    int found = -1, count = 0;
    for (int i = 0; i < n_pads; i++) {
        if (claimed[i] || !identity_matches_device(identity, i)) continue;
        found = i; count++;
    }
    return count == 1 ? found : -1;
}
static void remember_assigned_instances(void)
{
    for (int g = 0; g < N_GAMES; g++) {
        for (int player = 0; player < MAX_MP_PLAYERS; player++) {
            int device = settings.g[g].pad_device[player];
            SDL_JoystickID instance = device_instance(device);
            if (instance >= 0) assigned_instance[g][player] = instance;
        }
    }
}
#ifdef __ANDROID__
static int android_java_vibrator_count_cache = -1;

static JNIEnv *pad_android_env_and_activity(jobject *activity)
{
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    if (!env || !activity) return NULL;
    *activity = (jobject)SDL_AndroidGetActivity();
    if (!*activity) return NULL;
    return env;
}

static int pad_android_java_vibrator_count(void)
{
    if (android_java_vibrator_count_cache >= 0) return android_java_vibrator_count_cache;
    jobject activity = NULL;
    JNIEnv *env = pad_android_env_and_activity(&activity);
    if (!env) return 0;
    jclass cls = (*env)->GetObjectClass(env, activity);
    jmethodID method = cls ? (*env)->GetMethodID(env, cls, "pipeCleanVibratorCount", "()I") : NULL;
    jint count = method ? (*env)->CallIntMethod(env, activity, method) : 0;
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); count = 0; }
    if (cls) (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, activity);
    android_java_vibrator_count_cache = count > 0 ? (int)count : 0;
    return android_java_vibrator_count_cache;
}

static void pad_android_java_vibrator_name(int slot, char *buf, size_t n)
{
    if (!buf || n == 0) return;
    snprintf(buf, n, "Phone actuator %d", slot + 1);
    jobject activity = NULL;
    JNIEnv *env = pad_android_env_and_activity(&activity);
    if (!env) return;
    jclass cls = (*env)->GetObjectClass(env, activity);
    jmethodID method = cls ? (*env)->GetMethodID(env, cls, "pipeCleanVibratorName", "(I)Ljava/lang/String;") : NULL;
    jstring result = method ? (jstring)(*env)->CallObjectMethod(env, activity, method, (jint)slot) : NULL;
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); result = NULL; }
    if (result) {
        const char *utf = (*env)->GetStringUTFChars(env, result, NULL);
        if (utf) {
            snprintf(buf, n, "%s", utf);
            (*env)->ReleaseStringUTFChars(env, result, utf);
        }
        (*env)->DeleteLocalRef(env, result);
    }
    if (cls) (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, activity);
}

static int pad_android_java_vibrator_pulse(int slot, int ms, int amplitude)
{
    jobject activity = NULL;
    JNIEnv *env = pad_android_env_and_activity(&activity);
    if (!env) return 0;
    jclass cls = (*env)->GetObjectClass(env, activity);
    jmethodID method = cls ? (*env)->GetMethodID(env, cls, "pipeCleanPulseVibrator", "(III)Z") : NULL;
    jboolean ok = method ? (*env)->CallBooleanMethod(env, activity, method,
                        (jint)slot, (jint)ms, (jint)amplitude) : JNI_FALSE;
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); ok = JNI_FALSE; }
    if (cls) (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, activity);
    return ok == JNI_TRUE;
}

static void pad_android_java_vibrator_cancel(int slot)
{
    jobject activity = NULL;
    JNIEnv *env = pad_android_env_and_activity(&activity);
    if (!env) return;
    jclass cls = (*env)->GetObjectClass(env, activity);
    jmethodID method = cls ? (*env)->GetMethodID(env, cls, "pipeCleanCancelVibrator", "(I)V") : NULL;
    if (method) (*env)->CallVoidMethod(env, activity, method, (jint)slot);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    if (cls) (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, activity);
}

static void pad_close_haptic(Pad *p)
{
    if (!p || !p->haptic) return;
    SDL_HapticRumbleStop(p->haptic);
    SDL_HapticClose(p->haptic);
    p->haptic = NULL;
}

static void pad_open_haptic(int device)
{
    if (device < 0 || device >= n_pads) return;
    Pad *p = &pads[device];
    SDL_Joystick *j = joystick_for_device(device);
    if (!j) return;

    /* Prefer the vibration motor attached to this exact Android input device
     * (e.g. a USB/Bluetooth controller that exposes its own vibrator). */
    SDL_Haptic *h = SDL_HapticOpenFromJoystick(j);
    if (!h) return;
    if (!SDL_HapticRumbleSupported(h) || SDL_HapticRumbleInit(h) < 0) {
        SDL_HapticClose(h);
        return;
    }
    p->haptic = h;
}

static void pad_open_android_system_haptic(void)
{
    if (android_system_haptic) return;
    const int count = SDL_NumHaptics();
    for (int i = 0; i < count; ++i) {
        const char *name = SDL_HapticName(i);
        if (!name || strcmp(name, "VIBRATOR_SERVICE") != 0) continue;
        SDL_Haptic *h = SDL_HapticOpen(i);
        if (!h) continue;
        if (!SDL_HapticRumbleSupported(h) || SDL_HapticRumbleInit(h) < 0) {
            SDL_HapticClose(h);
            continue;
        }
        android_system_haptic = h;
        return;
    }
}
#endif

static void rescan(void)
{
    for (int i = 0; i < n_pads; i++) {
#ifdef __ANDROID__
        pad_close_haptic(&pads[i]);
#endif
        if (pads[i].gc) SDL_GameControllerClose(pads[i].gc);
        else if (pads[i].joy) SDL_JoystickClose(pads[i].joy);
    }
    memset(pads, 0, sizeof pads);
    n_pads = 0;
#ifdef __ANDROID__
    /* Refresh SDL's Android InputDevice vibrator list before binding the
     * newly arrived joystick to its per-device haptic (SDL polls this list). */
    (void)SDL_NumHaptics();
#endif
    int nj = SDL_NumJoysticks();
    for (int i = 0; i < nj && n_pads < MAXPADS; i++) {
        if (SDL_IsGameController(i)) {
            SDL_GameController *controller = SDL_GameControllerOpen(i);
            if (controller) {
                pads[n_pads].gc = controller;
                pads[n_pads].mapped = 1;
                n_pads++;
#ifdef __ANDROID__
                pad_open_haptic(n_pads - 1);
#endif
            }
        } else {
            SDL_Joystick *joystick = SDL_JoystickOpen(i);
            if (joystick) {
                pads[n_pads].joy = joystick;
                n_pads++;
#ifdef __ANDROID__
                pad_open_haptic(n_pads - 1);
#endif
            }
        }
    }
#ifdef __ANDROID__
    pad_open_android_system_haptic();
    if (settings.android_haptic_target >= ANDROID_HAPTIC_CONTROLLER_FIRST + n_pads) {
        settings.android_haptic_target = ANDROID_HAPTIC_AUTO;
        settings_save();
    }
#endif
}

void pad_init(void)
{
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4_RUMBLE, "1");
#ifdef __ANDROID__
    /* Android SDL2 implements vibration through SDL_Haptic, not through the
     * SDL_JoystickRumble backend (which returns unsupported on this platform). */
    SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK | SDL_INIT_HAPTIC);
#else
    SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK);
#endif
    for (int g = 0; g < N_GAMES; g++)
        for (int player = 0; player < MAX_MP_PLAYERS; player++)
            assigned_instance[g][player] = (SDL_JoystickID)-1;
    sync_initialized = 0;
    rescan();
    pad_sync_assignments();
    /* If a DualSense is already connected when PipeClean starts, open its
       audio endpoint now so event speaker effects work without first visiting
       the DualSense settings page. */
    for (int i = 0; i < n_pads; i++) {
        if (pad_is_dualsense(i)) {
            (void)audio_pad_open();
            break;
        }
    }
}

void pad_shutdown(void)
{
    for (int i = 0; i < n_pads; i++) {
#ifdef __ANDROID__
        pad_close_haptic(&pads[i]);
#endif
        if (pads[i].gc) SDL_GameControllerClose(pads[i].gc);
        else if (pads[i].joy) SDL_JoystickClose(pads[i].joy);
    }
    memset(pads, 0, sizeof pads);
    n_pads = 0;
#ifdef __ANDROID__
    if (android_system_haptic) {
        SDL_HapticRumbleStop(android_system_haptic);
        SDL_HapticClose(android_system_haptic);
        android_system_haptic = NULL;
    }
#endif
    audio_pad_close();
#ifdef __ANDROID__
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK | SDL_INIT_HAPTIC);
#else
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK);
#endif
}

void pad_event(const SDL_Event *e)
{
    if (e->type == SDL_CONTROLLERDEVICEADDED || e->type == SDL_CONTROLLERDEVICEREMOVED ||
        e->type == SDL_JOYDEVICEADDED || e->type == SDL_JOYDEVICEREMOVED) {
        /* Snapshot live player-to-instance ownership before device indices change. */
        remember_assigned_instances();
        rescan();
        pad_sync_assignments();
        if (e->type == SDL_CONTROLLERDEVICEADDED) {
            for (int i = 0; i < n_pads; i++) {
                if (pad_is_dualsense(i)) {
                    (void)audio_pad_open();
                    break;
                }
            }
        }
    }
}

int pad_device_identity(int device, char *buf, size_t n)
{
    SDL_Joystick *j = joystick_for_device(device);
    if (!buf || !n) return 0;
    buf[0] = 0;
    if (!j) return 0;

    /* Prefer per-physical-device metadata. A GUID alone identifies only a model. */
    const char *serial = SDL_JoystickGetSerial(j);
    if (serial && serial[0]) {
        snprintf(buf, n, "S:%s", serial);
        return buf[0] != 0;
    }
    const char *path = SDL_JoystickPath(j);
    if (path && path[0]) {
        snprintf(buf, n, "P:%s", path);
        return buf[0] != 0;
    }

    char guid[40];
    if (!pad_device_raw_guid(device, guid, sizeof guid)) return 0;
    snprintf(buf, n, "G:%s|%s", guid, name_of(&pads[device]));
    return buf[0] != 0;
}

void pad_sync_assignments(void)
{
    for (int g = 0; g < N_GAMES; g++) {
        GameCfg *c = &settings.g[g];
        int chosen[MAX_MP_PLAYERS] = {-1, -1, -1, -1};
        int claimed[MAXPADS] = {0, 0, 0, 0};

        /* First preserve the actual live joystick instance for each player. */
        for (int player = 0; player < MAX_MP_PLAYERS; player++) {
            SDL_JoystickID instance = assigned_instance[g][player];
            int device = pad_find_instance(instance);
            if (instance >= 0 && device >= 0 && !claimed[device]) {
                chosen[player] = device;
                claimed[device] = 1;
            } else if (instance >= 0) {
                assigned_instance[g][player] = (SDL_JoystickID)-1;
            }
        }

        /* Serial/path identities reconnect safely even if SDL changes slot order. */
        for (int player = 0; player < MAX_MP_PLAYERS; player++) {
            if (chosen[player] >= 0 || !identity_is_strong(c->pad_guid[player])) continue;
            int device = unique_unclaimed_identity(c->pad_guid[player], claimed);
            if (device >= 0) {
                chosen[player] = device;
                claimed[device] = 1;
                assigned_instance[g][player] = device_instance(device);
            }
        }

        /* On first launch, resolve duplicated legacy GUIDs only when safe. */
        if (!sync_initialized) {
            for (int player = 0; player < MAX_MP_PLAYERS; player++) {
                if (chosen[player] >= 0) continue;
                char guid[33];
                if (!identity_weak_guid(c->pad_guid[player], guid)) continue;
                int owners = 0, owner_other = -1;
                for (int other = 0; other < MAX_MP_PLAYERS; other++) {
                    char other_guid[33];
                    if (identity_weak_guid(c->pad_guid[other], other_guid) && !strcmp(guid, other_guid)) {
                        owners++;
                        if (other != player) owner_other = other;
                    }
                }
                if (owners <= 1) continue;
                int total = weak_guid_candidate_count(guid, NULL);
                if (total >= owners) {
                    for (int other = 0; other < MAX_MP_PLAYERS; other++) {
                        char other_guid[33];
                        if (chosen[other] >= 0 || !identity_weak_guid(c->pad_guid[other], other_guid) || strcmp(guid, other_guid)) continue;
                        int hint = c->pad_device[other];
                        char got[40];
                        if (hint >= 0 && hint < n_pads && !claimed[hint] &&
                            pad_device_raw_guid(hint, got, sizeof got) && !strcmp(guid, got)) {
                            chosen[other] = hint;
                            claimed[hint] = 1;
                            assigned_instance[g][other] = device_instance(hint);
                        }
                    }
                } else if (owner_other >= 0 && c->pad_device[player] >= 0 &&
                           c->pad_device[owner_other] < 0) {
                    int hint = c->pad_device[player];
                    char got[40];
                    if (hint < n_pads && !claimed[hint] &&
                        pad_device_raw_guid(hint, got, sizeof got) && !strcmp(guid, got)) {
                        chosen[player] = hint;
                        claimed[hint] = 1;
                        assigned_instance[g][player] = device_instance(hint);
                    }
                }
            }
        }

        /* Migrate older configs that never stored a device identity. */
        if (!sync_initialized) {
            for (int player = 0; player < MAX_MP_PLAYERS; player++) {
                if (chosen[player] >= 0 || c->pad_guid[player][0]) continue;
                int hint = c->pad_device[player];
                if (hint >= 0 && hint < n_pads && !claimed[hint]) {
                    chosen[player] = hint;
                    claimed[hint] = 1;
                    assigned_instance[g][player] = device_instance(hint);
                }
            }
        }

        /* Weak GUID matching is safe only if ownership is unambiguous or every
           other player with that GUID is already attached to their live instance. */
        for (int player = 0; player < MAX_MP_PLAYERS; player++) {
            if (chosen[player] >= 0) continue;
            char guid[33];
            if (!identity_weak_guid(c->pad_guid[player], guid)) continue;
            int other_owners = 0, all_other_owners_live = 1;
            for (int other = 0; other < MAX_MP_PLAYERS; other++) {
                char other_guid[33];
                if (other == player || !identity_weak_guid(c->pad_guid[other], other_guid) || strcmp(guid, other_guid)) continue;
                other_owners++;
                if (chosen[other] < 0) all_other_owners_live = 0;
            }
            int available = weak_guid_candidate_count(guid, claimed);
            if (other_owners == 0 || all_other_owners_live) {
                if (available == 1) {
                    for (int d = 0; d < n_pads; d++) {
                        if (!claimed[d] && identity_matches_device(c->pad_guid[player], d)) {
                            chosen[player] = d;
                            claimed[d] = 1;
                            assigned_instance[g][player] = device_instance(d);
                            break;
                        }
                    }
                }
            }
        }

        for (int player = 0; player < MAX_MP_PLAYERS; player++) {
            c->pad_device[player] = chosen[player];
            if (chosen[player] < 0) {
                assigned_instance[g][player] = (SDL_JoystickID)-1;
                continue;
            }
            assigned_instance[g][player] = device_instance(chosen[player]);
            char current[512];
            if (pad_device_identity(chosen[player], current, sizeof current)) {
                if (!c->pad_guid[player][0] ||
                    (identity_is_strong(current) && !identity_is_strong(c->pad_guid[player])) ||
                    (strlen(c->pad_guid[player]) == 32 && !identity_is_strong(current)))
                    snprintf(c->pad_guid[player], sizeof c->pad_guid[player], "%s", current);
            }
        }
    }
    sync_initialized = 1;
}

int pad_assign_device(int game, int player, int device)
{
    if (game < 0 || game >= N_GAMES || player < 0 || player >= MAX_MP_PLAYERS) return 0;
    if (device < -1 || device >= n_pads) return 0;
    GameCfg *c = &settings.g[game];
    if (device >= 0) {
        for (int other = 0; other < MAX_MP_PLAYERS; other++)
            if (other != player && c->pad_device[other] == device) return 0;
        char identity[512];
        if (!pad_device_identity(device, identity, sizeof identity)) return 0;
        c->pad_device[player] = device;
        snprintf(c->pad_guid[player], sizeof c->pad_guid[player], "%s", identity);
        assigned_instance[game][player] = device_instance(device);
    } else {
        c->pad_device[player] = -1;
        c->pad_guid[player][0] = 0;
        assigned_instance[game][player] = (SDL_JoystickID)-1;
    }
    return 1;
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

void pad_set_touch_input(int game, int player, uint8_t buttons, uint8_t dpad)
{
    if (game < 0 || game >= N_GAMES || player < 0 || player >= MAX_MP_PLAYERS) return;
    touch_buttons[game][player] = buttons & 0x0Fu;
    touch_dpad[game][player] = dpad & 0x0Fu;
}

static void pad_poll_one(int game, const GameCfg *c, int player, uint8_t *b, uint8_t *d)
{
    const Uint8 *ks = SDL_GetKeyboardState(NULL);
    uint8_t bits[N_BTN] = {0};
    if (player < 0 || player >= MAX_MP_PLAYERS) player = 0;

    /* The GBC Wario Land II profile may have been created after the user's
     * controller was assigned to the GB release. If its per-game assignment
     * is missing, reuse that sibling profile's live device rather than
     * silently making the controller unusable for this title. An explicit,
     * valid GBC-specific assignment still takes precedence. */
    int device = c->pad_device[player];
    if ((device < 0 || device >= n_pads) &&
        game == GAME_WARIO_LAND2_GBC && player == 0) {
        int sibling_device = settings.g[GAME_WARIO_LAND2_GB].pad_device[0];
        if (sibling_device >= 0 && sibling_device < n_pads)
            device = sibling_device;
    }

    for (int i = 0; i < N_BTN; i++) {
        if (c->key[i][player]) {
            SDL_Scancode sc = SDL_GetScancodeFromKey(c->key[i][player]);
            if (sc != SDL_SCANCODE_UNKNOWN && ks[sc]) bits[i] = 1;
        }
        if (c->pad[i][player] >= 0 && pad_down(device, c->pad[i][player])) bits[i] = 1;
    }

    /* All keyboard input comes from the configurable per-player bindings. */
    float dz = settings.pad_deadzone / 100.0f * 32767.0f;
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
    uint8_t tb = touch_buttons[game][player], td = touch_dpad[game][player];
    bits[BTN_A] |= (tb & 0x01u) != 0;
    bits[BTN_B] |= (tb & 0x02u) != 0;
    bits[BTN_SELECT] |= (tb & 0x04u) != 0;
    bits[BTN_START] |= (tb & 0x08u) != 0;
    bits[BTN_RIGHT] |= (td & 0x01u) != 0;
    bits[BTN_LEFT] |= (td & 0x02u) != 0;
    bits[BTN_UP] |= (td & 0x04u) != 0;
    bits[BTN_DOWN] |= (td & 0x08u) != 0;
    *b = (uint8_t)(bits[BTN_A] | bits[BTN_B] << 1 | bits[BTN_SELECT] << 2 | bits[BTN_START] << 3);
    *d = (uint8_t)(bits[BTN_RIGHT] | bits[BTN_LEFT] << 1 | bits[BTN_UP] << 2 | bits[BTN_DOWN] << 3);
}

void pad_poll_player(int game, int player, uint8_t *b, uint8_t *d)
{
    if (!b || !d || game < 0) return;
    if (game >= N_GAMES) { *b = 0; *d = 0; return; }
    pad_poll_one(game, &settings.g[game], player, b, d);
    if ((*d & 3) == 3) *d &= (uint8_t)~3;
    if ((*d & 12) == 12) *d &= (uint8_t)~12;
}

void pad_poll(int game, uint8_t *b, uint8_t *d)
{
    /*
     * The normal single-player loop has exactly one Game Boy input port.
     * Never OR Player 2 into this state: it lets the second controller or
     * Keyboard 2 move/control Player 1 in non-multiplayer games.
     *
     * SML1 local multiplayer polls pad_poll_player() independently for both
     * emulated player states, so its Player 2 support remains unaffected.
     */
    if (!b || !d || game < 0 || game >= N_GAMES) {
        if (b) *b = 0;
        if (d) *d = 0;
        return;
    }
    pad_poll_player(game, 0, b, d);
}

/* Convert the left stick into cursor velocity while suppressing stick drift. */
static float launcher_axis_value(Sint16 raw)
{
    float v = raw < 0 ? (float)raw / 32768.0f : (float)raw / 32767.0f;
    float a = fabsf(v);
    if (a <= 0.20f) return 0.0f;
    return (v < 0.0f ? -1.0f : 1.0f) * ((a - 0.20f) / 0.80f);
}

void pad_poll_launcher(float *x_axis, float *y_axis, int *confirm, int *back)
{
    if (x_axis) *x_axis = 0.0f;
    if (y_axis) *y_axis = 0.0f;
    if (confirm) *confirm = 0;
    if (back) *back = 0;

    float best_x = 0.0f, best_y = 0.0f;
    int any_confirm = 0, any_back = 0;

    /* Any connected controller can use the launcher before being assigned
       to a game's Player 1 slot. */
    for (int i = 0; i < n_pads; i++) {
        Pad *p = &pads[i];
        float ax = 0.0f, ay = 0.0f;
        int a = 0, b = 0;
        int left = 0, right = 0, up = 0, down = 0;

        if (p->mapped) {
            ax = launcher_axis_value(SDL_GameControllerGetAxis(p->gc, SDL_CONTROLLER_AXIS_LEFTX));
            ay = launcher_axis_value(SDL_GameControllerGetAxis(p->gc, SDL_CONTROLLER_AXIS_LEFTY));
            a = SDL_GameControllerGetButton(p->gc, SDL_CONTROLLER_BUTTON_A) != 0;
            b = SDL_GameControllerGetButton(p->gc, SDL_CONTROLLER_BUTTON_B) != 0;
            left  = SDL_GameControllerGetButton(p->gc, SDL_CONTROLLER_BUTTON_DPAD_LEFT) != 0;
            right = SDL_GameControllerGetButton(p->gc, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) != 0;
            up    = SDL_GameControllerGetButton(p->gc, SDL_CONTROLLER_BUTTON_DPAD_UP) != 0;
            down  = SDL_GameControllerGetButton(p->gc, SDL_CONTROLLER_BUTTON_DPAD_DOWN) != 0;
        } else {
            SDL_Joystick *j = p->joy;
            if (SDL_JoystickNumAxes(j) > 0) ax = launcher_axis_value(SDL_JoystickGetAxis(j, 0));
            if (SDL_JoystickNumAxes(j) > 1) ay = launcher_axis_value(SDL_JoystickGetAxis(j, 1));
            a = SDL_JoystickNumButtons(j) > 0 && SDL_JoystickGetButton(j, 0) != 0;
            b = SDL_JoystickNumButtons(j) > 1 && SDL_JoystickGetButton(j, 1) != 0;
            if (SDL_JoystickNumHats(j) > 0) {
                Uint8 hat = SDL_JoystickGetHat(j, 0);
                left = (hat & SDL_HAT_LEFT) != 0;
                right = (hat & SDL_HAT_RIGHT) != 0;
                up = (hat & SDL_HAT_UP) != 0;
                down = (hat & SDL_HAT_DOWN) != 0;
            }
        }

        if (left != right) ax = left ? -1.0f : 1.0f;
        if (up != down) ay = up ? -1.0f : 1.0f;
        if (fabsf(ax) > fabsf(best_x)) best_x = ax;
        if (fabsf(ay) > fabsf(best_y)) best_y = ay;
        any_confirm |= a;
        any_back |= b;
    }

    if (x_axis) *x_axis = best_x;
    if (y_axis) *y_axis = best_y;
    if (confirm) *confirm = any_confirm;
    if (back) *back = any_back;
}


int pad_is_selected_instance(int game, SDL_JoystickID which)
{
    if (game < 0 || game >= N_GAMES) return 0;
    int device = settings.g[game].pad_device[0];
    return device >= 0 && device < n_pads && pad_instance_device(which) == device;
}

int pad_is_selected_dualsense_instance(int game, SDL_JoystickID which)
{
    if (!pad_is_selected_instance(game, which)) return 0;
    int device = settings.g[game].pad_device[0];
    return pad_is_dualsense(device);
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
static float flash_duration = 0.40f;
static uint32_t last_sent = 0xFFFFFFFFu;
static float send_acc;

static void pad_cancel_sml2_feedback(void);
static void pad_cancel_sml2_light(void);

void pad_set_context(int game, int in_game)
{
    if (ctx_in_game && (!in_game || game != ctx_game))
        pad_cancel_sml2_feedback();
    ctx_game = game;
    ctx_in_game = in_game;
    last_sent = 0xFFFFFFFFu;
}
void pad_set_screen_color(uint32_t rgb) { screen_rgb = rgb; }
void pad_flash(uint32_t rgb)
{
    pad_cancel_sml2_light();
    flash_rgb = rgb;
    flash_t = 1.0f;
    flash_duration = 0.40f;
}

int pad_has_light(void)
{
    /* Only DualSense lightbars are driven by PipeClean's event lighting. */
    for (int i = 0; i < n_pads; i++)
        if (pad_is_dualsense(i) && SDL_GameControllerHasLED(pads[i].gc)) return 1;
    return 0;
}

static int pad_device_has_rumble(int device)
{
    if (device < 0 || device >= n_pads) return 0;
    Pad *p = &pads[device];
#ifdef __ANDROID__
    if (p->haptic) return 1;
#endif
    if (p->mapped && SDL_GameControllerHasRumble(p->gc)) return 1;
    SDL_Joystick *j = joystick_for_device(device);
    return j && SDL_JoystickHasRumble(j);
}

static int pad_try_controller_rumble(int device, int low_pct, int high_pct, int ms)
{
    if (device < 0 || device >= n_pads || ms <= 0) return 0;
    const Uint16 low = (Uint16)((unsigned)low_pct * 65535u / 100u);
    const Uint16 high = (Uint16)((unsigned)high_pct * 65535u / 100u);
    Pad *p = &pads[device];

    /* Prefer SDL's native controller backend. On Android this routes to the
       controller's InputDevice vibrator(s), rather than the phone vibrator. */
    if (p->mapped && SDL_GameControllerHasRumble(p->gc) &&
        SDL_GameControllerRumble(p->gc, low, high, (Uint32)ms) == 0)
        return 1;

    SDL_Joystick *j = joystick_for_device(device);
    if (j && SDL_JoystickHasRumble(j) &&
        SDL_JoystickRumble(j, low, high, (Uint32)ms) == 0)
        return 1;

#ifdef __ANDROID__
    /* Compatibility path for SDL haptic devices exposed directly from a pad. */
    const int strongest = low_pct > high_pct ? low_pct : high_pct;
    if (p->haptic &&
        SDL_HapticRumblePlay(p->haptic, strongest / 100.0f, (Uint32)ms) == 0)
        return 1;
#endif
    return 0;
}

static int pad_rumble_device_mix(int device, int low_pct, int high_pct, int ms)
{
    if ((low_pct <= 0 && high_pct <= 0) || ms <= 0) return 0;
    if (low_pct < 0) low_pct = 0;
    if (high_pct < 0) high_pct = 0;
    if (low_pct > 100) low_pct = 100;
    if (high_pct > 100) high_pct = 100;

#ifdef __ANDROID__
    const int target = settings.android_haptic_target;
    const int strongest = low_pct > high_pct ? low_pct : high_pct;

    /* Explicit phone mode never vibrates a connected controller. This also
       works when no physical controller is assigned to Player 1. */
    if (target == ANDROID_HAPTIC_PHONE) {
        if (pad_android_java_vibrator_pulse(-1, ms, strongest * 255 / 100)) return 1;
        return android_system_haptic &&
               SDL_HapticRumblePlay(android_system_haptic,
                                    strongest / 100.0f, (Uint32)ms) == 0;
    }

    if (target >= ANDROID_HAPTIC_PHONE_MOTOR_FIRST &&
        target < ANDROID_HAPTIC_CONTROLLER_FIRST) {
        const int slot = target - ANDROID_HAPTIC_PHONE_MOTOR_FIRST;
        return pad_android_java_vibrator_pulse(slot, ms, strongest * 255 / 100);
    }

    /* A numbered controller route is strict for diagnostics: don't fall back
       to the phone, because that could make a broken controller look healthy. */
    if (target >= ANDROID_HAPTIC_CONTROLLER_FIRST) {
        const int selected = target - ANDROID_HAPTIC_CONTROLLER_FIRST;
        return pad_try_controller_rumble(selected, low_pct, high_pct, ms);
    }

    if (pad_try_controller_rumble(device, low_pct, high_pct, ms)) return 1;
    if (pad_android_java_vibrator_pulse(-1, ms, strongest * 255 / 100)) return 1;
    if (android_system_haptic &&
        SDL_HapticRumblePlay(android_system_haptic,
                             strongest / 100.0f, (Uint32)ms) == 0)
        return 1;
    return 0;
#else
    return pad_try_controller_rumble(device, low_pct, high_pct, ms);
#endif
}

static void pad_stop_controller_rumble(int device)
{
    if (device < 0 || device >= n_pads) return;
    Pad *p = &pads[device];
#ifdef __ANDROID__
    if (p->haptic) SDL_HapticRumbleStop(p->haptic);
#endif
    if (p->mapped && SDL_GameControllerHasRumble(p->gc))
        (void)SDL_GameControllerRumble(p->gc, 0, 0, 0);
    SDL_Joystick *j = joystick_for_device(device);
    if (j && SDL_JoystickHasRumble(j))
        (void)SDL_JoystickRumble(j, 0, 0, 0);
}

static void pad_stop_rumble_device(int device)
{
#ifdef __ANDROID__
    /* The phone motor is a shared system output, and the sequence's controller
       index may be -1 when the game is being played with touch controls. */
    if (android_system_haptic) SDL_HapticRumbleStop(android_system_haptic);
    if (settings.android_haptic_target == ANDROID_HAPTIC_PHONE)
        pad_android_java_vibrator_cancel(-1);
    else if (settings.android_haptic_target >= ANDROID_HAPTIC_PHONE_MOTOR_FIRST &&
             settings.android_haptic_target < ANDROID_HAPTIC_CONTROLLER_FIRST)
        pad_android_java_vibrator_cancel(settings.android_haptic_target -
                                         ANDROID_HAPTIC_PHONE_MOTOR_FIRST);
    else if (settings.android_haptic_target >= ANDROID_HAPTIC_CONTROLLER_FIRST)
        pad_stop_controller_rumble(settings.android_haptic_target -
                                   ANDROID_HAPTIC_CONTROLLER_FIRST);
    else {
        pad_android_java_vibrator_cancel(-1);
        pad_stop_controller_rumble(device);
    }
#else
    pad_stop_controller_rumble(device);
#endif
}

static void pad_rumble_device(int device, int pct, int ms)
{
    pad_rumble_device_mix(device, pct, pct * 80 / 100, ms);
}

void pad_rumble(int pct, int ms)
{
    if (pct <= 0) return;
    for (int i = 0; i < n_pads; i++) pad_rumble_device(i, pct, ms);
}

void pad_rumble_selected(int game, int pct, int ms)
{
    if (game < 0 || game >= N_GAMES) return;
    pad_rumble_device(settings.g[game].pad_device[0], pct, ms);
}

int pad_android_haptic_option_count(void)
{
#ifdef __ANDROID__
    int phone_motors = pad_android_java_vibrator_count();
    if (phone_motors <= 1) phone_motors = 0;
    if (phone_motors > ANDROID_HAPTIC_PHONE_MOTOR_SLOTS)
        phone_motors = ANDROID_HAPTIC_PHONE_MOTOR_SLOTS;
    return 2 + phone_motors + n_pads;
#else
    return 0;
#endif
}

int pad_android_haptic_option_target(int option)
{
#ifdef __ANDROID__
    if (option == 0) return ANDROID_HAPTIC_AUTO;
    if (option == 1) return ANDROID_HAPTIC_PHONE;
    int phone_motors = pad_android_java_vibrator_count();
    if (phone_motors <= 1) phone_motors = 0;
    if (phone_motors > ANDROID_HAPTIC_PHONE_MOTOR_SLOTS)
        phone_motors = ANDROID_HAPTIC_PHONE_MOTOR_SLOTS;
    if (option >= 2 && option < 2 + phone_motors)
        return ANDROID_HAPTIC_PHONE_MOTOR_FIRST + option - 2;
    int controller = option - (2 + phone_motors);
    if (controller >= 0 && controller < n_pads)
        return ANDROID_HAPTIC_CONTROLLER_FIRST + controller;
#else
    (void)option;
#endif
    return -1;
}

int pad_android_haptic_target_option(int target)
{
#ifdef __ANDROID__
    if (target == ANDROID_HAPTIC_AUTO) return 0;
    if (target == ANDROID_HAPTIC_PHONE) return 1;
    int phone_motors = pad_android_java_vibrator_count();
    if (phone_motors <= 1) phone_motors = 0;
    if (phone_motors > ANDROID_HAPTIC_PHONE_MOTOR_SLOTS)
        phone_motors = ANDROID_HAPTIC_PHONE_MOTOR_SLOTS;
    if (target >= ANDROID_HAPTIC_PHONE_MOTOR_FIRST &&
        target < ANDROID_HAPTIC_CONTROLLER_FIRST) {
        int slot = target - ANDROID_HAPTIC_PHONE_MOTOR_FIRST;
        return slot < phone_motors ? 2 + slot : 0;
    }
    if (target >= ANDROID_HAPTIC_CONTROLLER_FIRST) {
        int controller = target - ANDROID_HAPTIC_CONTROLLER_FIRST;
        if (controller < n_pads) return 2 + phone_motors + controller;
    }
#else
    (void)target;
#endif
    return 0;
}

void pad_android_haptic_option_name(int option, char *buf, size_t n)
{
    if (!buf || n == 0) return;
    buf[0] = 0;
#ifdef __ANDROID__
    int target = pad_android_haptic_option_target(option);
    if (target == ANDROID_HAPTIC_AUTO) {
        snprintf(buf, n, "Automatic (controller, then phone)");
    } else if (target == ANDROID_HAPTIC_PHONE) {
        snprintf(buf, n, "Android phone vibrator (default)");
    } else if (target >= ANDROID_HAPTIC_PHONE_MOTOR_FIRST &&
               target < ANDROID_HAPTIC_CONTROLLER_FIRST) {
        pad_android_java_vibrator_name(target - ANDROID_HAPTIC_PHONE_MOTOR_FIRST, buf, n);
    } else if (target >= ANDROID_HAPTIC_CONTROLLER_FIRST) {
        snprintf(buf, n, "Controller: %.100s",
                 name_of(&pads[target - ANDROID_HAPTIC_CONTROLLER_FIRST]));
    } else {
        snprintf(buf, n, "Unavailable vibration output");
    }
#else
    (void)option;
    snprintf(buf, n, "Android only");
#endif
}

int pad_android_haptic_target_available(int target)
{
#ifdef __ANDROID__
    if (target == ANDROID_HAPTIC_AUTO) {
        if (pad_android_java_vibrator_count() > 0 || android_system_haptic) return 1;
        for (int i = 0; i < n_pads; i++) if (pad_device_has_rumble(i)) return 1;
        return 0;
    }
    if (target == ANDROID_HAPTIC_PHONE)
        return pad_android_java_vibrator_count() > 0 || android_system_haptic != NULL;
    if (target >= ANDROID_HAPTIC_PHONE_MOTOR_FIRST &&
        target < ANDROID_HAPTIC_CONTROLLER_FIRST)
        return target - ANDROID_HAPTIC_PHONE_MOTOR_FIRST < pad_android_java_vibrator_count();
    if (target >= ANDROID_HAPTIC_CONTROLLER_FIRST)
        return pad_device_has_rumble(target - ANDROID_HAPTIC_CONTROLLER_FIRST);
#else
    (void)target;
#endif
    return 0;
}

int pad_android_haptic_test(int game, int strength_pct, int ms)
{
#ifdef __ANDROID__
    if (game < 0 || game >= N_GAMES) return 0;
    const int device = settings.g[game].pad_device[0];
    return pad_rumble_device_mix(device, strength_pct, strength_pct * 80 / 100, ms);
#else
    (void)game; (void)strength_pct; (void)ms;
    return 0;
#endif
}

typedef struct {
    uint8_t low, high;
    uint16_t on_ms, gap_ms;
} Sml2RumbleBeat;

typedef struct {
    const Sml2RumbleBeat *beats;
    uint8_t count;
    uint8_t priority;
} Sml2RumbleProfile;

typedef struct {
    const Sml2RumbleBeat *beats;
    uint8_t count, next, priority;
    int device, scale;
    float wait_s;
    int active;
} Sml2RumbleSequence;

static const Sml2RumbleBeat sml2_rumble_coin[] = {
    {32, 28, 32, 0}
};
static const Sml2RumbleBeat sml2_rumble_enemy[] = {
    {42, 28, 42, 34}, {52, 58, 48, 0}
};
static const Sml2RumbleBeat sml2_rumble_life[] = {
    {74, 14, 130, 70}, {100, 22, 170, 85}, {80, 10, 230, 0}
};
static const Sml2RumbleBeat sml2_rumble_oneup[] = {
    {30, 42, 40, 32}, {42, 66, 55, 36}, {48, 82, 82, 0}
};
static const Sml2RumbleBeat sml2_rumble_power[] = {
    {32, 28, 42, 35}, {48, 56, 82, 0}
};
static const Sml2RumbleBeat sml2_rumble_star[] = {
    {38, 55, 42, 26}, {56, 86, 68, 32}, {40, 94, 96, 0}
};
static const Sml2RumbleBeat sml2_rumble_golden[] = {
    {48, 24, 72, 38}, {64, 44, 88, 42}, {78, 60, 112, 0}
};
static const Sml2RumbleBeat sml2_rumble_castle[] = {
    {70, 18, 150, 42}, {100, 34, 230, 70}, {86, 24, 270, 0}
};
static const Sml2RumbleBeat sml2_rumble_damage[] = {
    {62, 20, 75, 42}, {48, 12, 145, 0}
};

static const Sml2RumbleProfile sml2_rumble_profiles[9] = {
    {sml2_rumble_coin,   sizeof sml2_rumble_coin   / sizeof sml2_rumble_coin[0],   1},
    {sml2_rumble_enemy,  sizeof sml2_rumble_enemy  / sizeof sml2_rumble_enemy[0],  3},
    {sml2_rumble_life,   sizeof sml2_rumble_life   / sizeof sml2_rumble_life[0],   8},
    {sml2_rumble_oneup,  sizeof sml2_rumble_oneup  / sizeof sml2_rumble_oneup[0],  6},
    {sml2_rumble_power,  sizeof sml2_rumble_power  / sizeof sml2_rumble_power[0],  4},
    {sml2_rumble_star,   sizeof sml2_rumble_star   / sizeof sml2_rumble_star[0],   7},
    {sml2_rumble_golden, sizeof sml2_rumble_golden / sizeof sml2_rumble_golden[0], 9},
    {sml2_rumble_castle, sizeof sml2_rumble_castle / sizeof sml2_rumble_castle[0], 10},
    {sml2_rumble_damage, sizeof sml2_rumble_damage / sizeof sml2_rumble_damage[0], 7}
};
static Sml2RumbleSequence sml2_rumble_sequence;

typedef struct {
    uint32_t rgb;
    uint16_t on_ms, gap_ms;
} Sml2LightBeat;

typedef struct {
    const Sml2LightBeat *beats;
    uint8_t count;
} Sml2LightProfile;

typedef struct {
    const Sml2LightBeat *beats;
    uint8_t count, next, priority;
    float wait_s;
    int active;
} Sml2LightSequence;

static const Sml2LightBeat sml2_light_coin[] = {
    {0xFFD34Au, 150, 0}
};
static const Sml2LightBeat sml2_light_enemy[] = {
    {0x68FF9Au, 125, 42}, {0xB8FFD0u, 105, 0}
};
static const Sml2LightBeat sml2_light_life[] = {
    {0xFF2020u, 380, 95}, {0xC90020u, 280, 0}
};
static const Sml2LightBeat sml2_light_oneup[] = {
    {0x49FF8Au, 125, 52}, {0xFFF1A0u, 160, 42}, {0x66FF9Au, 230, 0}
};
static const Sml2LightBeat sml2_light_power[] = {
    {0xFF8CFFu, 160, 48}, {0xA8A0FFu, 150, 0}
};
static const Sml2LightBeat sml2_light_star[] = {
    {0xFFFFFFu, 85, 32}, {0xFFE06Au, 170, 38}, {0xFFFFFFu, 145, 0}
};
static const Sml2LightBeat sml2_light_golden[] = {
    {0xFFD34Au, 160, 58}, {0xFFF0A8u, 200, 58}, {0xFFD34Au, 270, 0}
};
static const Sml2LightBeat sml2_light_castle[] = {
    {0xFFFFFFu, 190, 48}, {0xFFD34Au, 310, 62}, {0xFFF1A0u, 430, 0}
};
static const Sml2LightBeat sml2_light_damage[] = {
    {0xFF6538u, 175, 45}, {0xFF3030u, 125, 0}
};
static const Sml2LightProfile sml2_light_profiles[9] = {
    {sml2_light_coin,   sizeof sml2_light_coin   / sizeof sml2_light_coin[0]},
    {sml2_light_enemy,  sizeof sml2_light_enemy  / sizeof sml2_light_enemy[0]},
    {sml2_light_life,   sizeof sml2_light_life   / sizeof sml2_light_life[0]},
    {sml2_light_oneup,  sizeof sml2_light_oneup  / sizeof sml2_light_oneup[0]},
    {sml2_light_power,  sizeof sml2_light_power  / sizeof sml2_light_power[0]},
    {sml2_light_star,   sizeof sml2_light_star   / sizeof sml2_light_star[0]},
    {sml2_light_golden, sizeof sml2_light_golden / sizeof sml2_light_golden[0]},
    {sml2_light_castle, sizeof sml2_light_castle / sizeof sml2_light_castle[0]},
    {sml2_light_damage, sizeof sml2_light_damage / sizeof sml2_light_damage[0]}
};
static Sml2LightSequence sml2_light_sequence;

static int sml2_event_priority(int ev)
{
    return ev >= 0 && ev < 9 ? sml2_rumble_profiles[ev].priority : 0;
}

static void pad_cancel_sml2_light(void)
{
    sml2_light_sequence.active = 0;
    sml2_light_sequence.priority = 0;
    sml2_light_sequence.next = 0;
    sml2_light_sequence.wait_s = 0;
    flash_t = 0;
}

static void pad_cancel_sml2_feedback(void)
{
    if (sml2_rumble_sequence.active)
        pad_stop_rumble_device(sml2_rumble_sequence.device);
    sml2_rumble_sequence.active = 0;
    sml2_rumble_sequence.priority = 0;
    sml2_rumble_sequence.next = 0;
    sml2_rumble_sequence.wait_s = 0;
    pad_cancel_sml2_light();
}

static void pad_schedule_sml2_rumble(int ev)
{
    if (ev < 0 || ev >= 9) return;
    const GameCfg *c = &settings.g[GAME_SML2];
    const Sml2RumbleProfile *profile = &sml2_rumble_profiles[ev];
    const int device = c->pad_device[0];
    if (c->ds_rumble <= 0) return;
#ifndef __ANDROID__
    if (device < 0 || device >= n_pads) return;
#else
    if (settings.android_haptic_target >= ANDROID_HAPTIC_CONTROLLER_FIRST &&
        settings.android_haptic_target - ANDROID_HAPTIC_CONTROLLER_FIRST >= n_pads)
        return;
#endif
    if (sml2_rumble_sequence.active &&
        profile->priority <= sml2_rumble_sequence.priority)
        return;
    if (sml2_rumble_sequence.active)
        pad_stop_rumble_device(sml2_rumble_sequence.device);

    sml2_rumble_sequence.beats = profile->beats;
    sml2_rumble_sequence.count = profile->count;
    sml2_rumble_sequence.next = 0;
    sml2_rumble_sequence.priority = profile->priority;
    sml2_rumble_sequence.device = device;
    sml2_rumble_sequence.scale = c->ds_rumble;
    sml2_rumble_sequence.wait_s = 0;
    sml2_rumble_sequence.active = 1;
}

static void pad_schedule_sml2_light(int ev)
{
    if (ev < 0 || ev >= 9) return;
    const GameCfg *c = &settings.g[GAME_SML2];
    const int device = c->pad_device[0];
    if (device < 0 || device >= n_pads || !pad_is_dualsense(device) ||
        !pads[device].mapped || !SDL_GameControllerHasLED(pads[device].gc))
        return;
    const Sml2LightProfile *profile = &sml2_light_profiles[ev];
    const int priority = sml2_event_priority(ev);
    if (sml2_light_sequence.active && priority <= sml2_light_sequence.priority)
        return;

    sml2_light_sequence.beats = profile->beats;
    sml2_light_sequence.count = profile->count;
    sml2_light_sequence.next = 0;
    sml2_light_sequence.priority = (uint8_t)priority;
    sml2_light_sequence.wait_s = 0;
    sml2_light_sequence.active = 1;
}

static void pad_sml2_rumble_tick(float dt)
{
    if (!sml2_rumble_sequence.active) return;
    if (sml2_rumble_sequence.wait_s > 0.0f) {
        sml2_rumble_sequence.wait_s -= dt;
        if (sml2_rumble_sequence.wait_s > 0.0f) return;
    }
    if (sml2_rumble_sequence.next >= sml2_rumble_sequence.count) {
        sml2_rumble_sequence.active = 0;
        sml2_rumble_sequence.priority = 0;
        return;
    }

    const Sml2RumbleBeat *beat =
        &sml2_rumble_sequence.beats[sml2_rumble_sequence.next++];
    const int low = beat->low * sml2_rumble_sequence.scale / 100;
    const int high = beat->high * sml2_rumble_sequence.scale / 100;
    pad_rumble_device_mix(sml2_rumble_sequence.device, low, high, beat->on_ms);
    sml2_rumble_sequence.wait_s =
        ((float)beat->on_ms + (float)beat->gap_ms) / 1000.0f;
}

static void pad_sml2_light_tick(float dt)
{
    if (!sml2_light_sequence.active) return;
    if (sml2_light_sequence.wait_s > 0.0f) {
        sml2_light_sequence.wait_s -= dt;
        if (sml2_light_sequence.wait_s > 0.0f) return;
    }
    if (sml2_light_sequence.next >= sml2_light_sequence.count) {
        sml2_light_sequence.active = 0;
        sml2_light_sequence.priority = 0;
        flash_t = 0;
        return;
    }

    const Sml2LightBeat *beat =
        &sml2_light_sequence.beats[sml2_light_sequence.next++];
    flash_rgb = beat->rgb;
    flash_t = 1.0f;
    flash_duration = (float)beat->on_ms / 1000.0f;
    sml2_light_sequence.wait_s =
        ((float)beat->on_ms + (float)beat->gap_ms) / 1000.0f;
}

void pad_event_fx(int game, int ev)
{
    const EventDef *d = events_def(game, ev);
    if (!d) return;
    const GameCfg *c = &settings.g[game];
    if (game == GAME_SML2) {
        if ((c->ds_ev_rumble >> ev) & 1) pad_schedule_sml2_rumble(ev);
        if ((c->ds_ev_led >> ev) & 1) pad_schedule_sml2_light(ev);
        return;
    }
    if ((c->ds_ev_rumble >> ev) & 1)
        pad_rumble_selected(game, c->ds_rumble * d->rumble_str / 100, d->rumble_ms);
    if (((c->ds_ev_led >> ev) & 1) &&
        c->pad_device[0] >= 0 && c->pad_device[0] < n_pads &&
        pad_is_dualsense(c->pad_device[0]))
        pad_flash(d->color);
}

void pad_event_preview(int game, int ev)
{
    const EventDef *d = events_def(game, ev);
    if (!d) return;
    if (game == GAME_SML2) {
        pad_schedule_sml2_rumble(ev);
        pad_schedule_sml2_light(ev);
        return;
    }
    pad_flash(d->color);
    pad_rumble_selected(game, settings.g[game].ds_rumble * d->rumble_str / 100,
                        d->rumble_ms);
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
    if (ctx_game < 0) return;
#ifndef __ANDROID__
    if (!n_pads) return;
#endif
    pad_sml2_light_tick(dt);
    pad_sml2_rumble_tick(dt);
    const GameCfg *c = &settings.g[ctx_game];
    uint32_t base = base_color(c);
    float k = c->ds_bright / 100.0f;
    float r = ((base >> 16) & 255) * k, g = ((base >> 8) & 255) * k, b = (base & 255) * k;
    if (flash_t > 0) {
        float f = flash_t * flash_t;
        r += (((flash_rgb >> 16) & 255) - r) * f; g += (((flash_rgb >> 8) & 255) - g) * f; b += ((flash_rgb & 255) - b) * f;
        flash_t -= dt / (flash_duration > 0.01f ? flash_duration : 0.40f);
        if (flash_t < 0) flash_t = 0;
    }
    uint32_t out = ((uint32_t)(r + 0.5f) << 16) | ((uint32_t)(g + 0.5f) << 8) | (uint32_t)(b + 0.5f);
    send_acc += dt;
    if (out == last_sent || send_acc < 0.033f) return;
    send_acc = 0;
    last_sent = out;
    int device = settings.g[ctx_game].pad_device[0];
    if (device >= 0 && device < n_pads && pad_is_dualsense(device) &&
        pads[device].mapped && SDL_GameControllerHasLED(pads[device].gc))
        SDL_GameControllerSetLED(pads[device].gc, (Uint8)(out >> 16), (Uint8)(out >> 8), (Uint8)out);
}