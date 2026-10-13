/* Application loop: launcher <-> game. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <errno.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#endif
#include "app.h"
#include "launcher.h"
#include "ui.h"
#include "settings.h"
#include "rom.h"
#include "cart.h"
#include "emu.h"
#include "netplay.h"
#include "render.h"
#include "bgimg.h"
#include "texpack.h"
#include "pad.h"
#include "audio.h"
#include "events.h"
#include "util.h"
#include "branding.h"
#include "dev_diag.h"
#include "widescreen.h"
#include <SDL_image.h>
#if defined(__ANDROID__)
#include <jni.h>
#include <SDL_system.h>
#endif

static SDL_Window *win;
static SDL_Renderer *ren;
static int win_w = 1100, win_h = 720;
static int companion_rewind_held;
static int companion_pipe_swap_active;
static int companion_pipe_swap_exit_pending;
static SDL_atomic_t companion_display_available;
static Uint32 companion_last_frame_tick;
static int companion_slots_dirty = 1;
static int game_session_active = -1;
static Uint64 game_session_started;

static void game_session_begin(int game)
{
    int next[N_RECENT_PLAYED];
    int count = 0;
    if (game < 0 || game >= N_GAMES) return;
    if (game_session_active >= 0) return;
    for (int i = 0; i < N_RECENT_PLAYED; i++) next[i] = -1;
    next[count++] = game;
    for (int i = 0; i < settings.recent_played_count && i < N_RECENT_PLAYED; i++) {
        int old = settings.recent_played[i];
        if (old < 0 || old >= N_GAMES || old == game) continue;
        int duplicate = 0;
        for (int j = 0; j < count; j++) if (next[j] == old) duplicate = 1;
        if (!duplicate && count < N_RECENT_PLAYED) next[count++] = old;
    }
    for (int i = 0; i < N_RECENT_PLAYED; i++) settings.recent_played[i] = next[i];
    settings.recent_played_count = count;
    game_session_active = game;
    game_session_started = SDL_GetPerformanceCounter();
}

static void game_session_end(void)
{
    if (game_session_active < 0 || game_session_active >= N_GAMES) return;
    Uint64 now = SDL_GetPerformanceCounter();
    Uint64 freq = SDL_GetPerformanceFrequency();
    if (freq && now >= game_session_started) {
        Uint64 elapsed = (now - game_session_started) / freq;
        int current = settings.game_play_seconds[game_session_active];
        if (elapsed > (Uint64)(2147483647 - current))
            settings.game_play_seconds[game_session_active] = 2147483647;
        else
            settings.game_play_seconds[game_session_active] = current + (int)elapsed;
    }
    game_session_active = -1;
    game_session_started = 0;
    settings_save();
}

#define PIPECLEAN_COMPANION_EVENT_CODE 0x5043
#define PIPECLEAN_COMPANION_SAVE 1
#define PIPECLEAN_COMPANION_LOAD 2
#define PIPECLEAN_COMPANION_PAUSE 3
#define PIPECLEAN_COMPANION_REWIND 4
#define PIPECLEAN_COMPANION_PALETTE 5
#define PIPECLEAN_COMPANION_FILTER 6
#define PIPECLEAN_COMPANION_SLOT_PREV 7
#define PIPECLEAN_COMPANION_SLOT_NEXT 8
#define PIPECLEAN_COMPANION_ITEM_USE 9
#define PIPECLEAN_COMPANION_CLOSE_GAME 12
#define PIPECLEAN_COMPANION_SLOT_SELECT_BASE 20

#if defined(__ANDROID__)
JNIEXPORT void JNICALL Java_com_pipeclean_app_PipeCleanActivity_nativeCompanionAction(
    JNIEnv *env, jclass cls, jint action, jint pressed)
{
    (void)env; (void)cls;
    SDL_Event event;
    SDL_zero(event);
    event.type = SDL_USEREVENT;
    event.user.code = PIPECLEAN_COMPANION_EVENT_CODE;
    event.user.data1 = (void *)(intptr_t)action;
    event.user.data2 = (void *)(intptr_t)pressed;
    (void)SDL_PushEvent(&event);
}

JNIEXPORT void JNICALL Java_com_pipeclean_app_PipeCleanActivity_nativeCompanionDisplayAvailable(
    JNIEnv *env, jclass cls, jint available)
{
    (void)env; (void)cls;
    SDL_AtomicSet(&companion_display_available, available ? 1 : 0);
}

#define PIPECLEAN_LAYOUT_PICKER_EVENT_CODE 0x5044
typedef struct {
    int game;
    char path[1024];
} CompanionLayoutPickerResult;

JNIEXPORT void JNICALL Java_com_pipeclean_app_PipeCleanActivity_nativeCompanionLayoutPickerResult(
    JNIEnv *env, jclass cls, jint kind, jint game, jstring path)
{
    (void)cls;
    const char *utf = path ? (*env)->GetStringUTFChars(env, path, NULL) : NULL;
    CompanionLayoutPickerResult *payload =
        (CompanionLayoutPickerResult *)calloc(1, sizeof *payload);
    if (!payload) {
        if (utf) (*env)->ReleaseStringUTFChars(env, path, utf);
        return;
    }
    payload->game = (int)game;
    if (utf) {
        snprintf(payload->path, sizeof payload->path, "%s", utf);
        (*env)->ReleaseStringUTFChars(env, path, utf);
    }
    SDL_Event event;
    SDL_zero(event);
    event.type = SDL_USEREVENT;
    event.user.code = PIPECLEAN_LAYOUT_PICKER_EVENT_CODE;
    event.user.data1 = (void *)(intptr_t)kind;
    event.user.data2 = payload;
    if (SDL_PushEvent(&event) < 0) free(payload);
}

static const char *companion_layout_asset_paths(const CompanionLayoutCfg *d, int i)
{
    if (!d) return "";
    if (i == 0) return d->background_path;
    if (i == 1) return d->button_texture_path;
    if (i == 2) return d->item_texture_path;
    if (i == 3) return d->decoration_path;
    if (i >= 4 && i < 4 + N_DECK_BUTTON_SOUNDS) return d->button_sound_path[i - 4];
    if (i == 10) return d->item_sound_path;
    if (i == 11) return d->empty_item_sound_path;
    return "";
}

/* Opens an Android SAF picker. The bundle manifest contains only portable
   layout values; asset paths are supplied separately so export can package them. */
void launcher_open_companion_layout_picker(int kind, int game)
{
    if (game < 0 || game >= N_GAMES) return;
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    jobject activity = (jobject)SDL_AndroidGetActivity();
    if (!env || !activity) return;
    jclass cls = (*env)->GetObjectClass(env, activity);
    if (!cls) return;

    CompanionLayoutCfg *d = &settings.deck_layout[game];
    char manifest[2048];
    snprintf(manifest, sizeof manifest,
        "layout_version=1\n"
        "header_dx=%d\nheader_dy=%d\nheader_scale=%d\n"
        "item_dx=%d\nitem_dy=%d\nitem_scale=%d\n"
        "controls_dx=%d\ncontrols_dy=%d\ncontrols_scale=%d\n"
        "timeline_dx=%d\ntimeline_dy=%d\ntimeline_scale=%d\n"
        "decoration_dx=%d\ndecoration_dy=%d\ndecoration_scale=%d\n"
        "sounds_enabled=%d\n",
        d->header_dx, d->header_dy, d->header_scale,
        d->item_dx, d->item_dy, d->item_scale,
        d->controls_dx, d->controls_dy, d->controls_scale,
        d->timeline_dx, d->timeline_dy, d->timeline_scale,
        d->decoration_dx, d->decoration_dy, d->decoration_scale,
        d->sounds_enabled);
    jstring text = (*env)->NewStringUTF(env, manifest);
    jclass string_class = (*env)->FindClass(env, "java/lang/String");
    jobjectArray paths = string_class
        ? (*env)->NewObjectArray(env, 12, string_class, NULL) : NULL;
    if (paths) {
        for (int i = 0; i < 12; i++) {
            jstring path = (*env)->NewStringUTF(env, companion_layout_asset_paths(d, i));
            if (path) {
                (*env)->SetObjectArrayElement(env, paths, i, path);
                (*env)->DeleteLocalRef(env, path);
            }
        }
    }
    jmethodID method = (*env)->GetMethodID(
        env, cls, "openCompanionLayoutPicker", "(IILjava/lang/String;[Ljava/lang/String;)V");
    if (method && text && paths) {
        /* Activity chooses a suitable title based on kind. */
        (void)(*env)->CallVoidMethod(env, activity, method, (jint)kind, (jint)game, text, paths);
    }
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    if (paths) (*env)->DeleteLocalRef(env, paths);
    if (string_class) (*env)->DeleteLocalRef(env, string_class);
    if (text) (*env)->DeleteLocalRef(env, text);
    (*env)->DeleteLocalRef(env, cls);
}

static void companion_publish_state(int game, const Frame *frame, int paused)
{
    static unsigned tick;
    if (++tick < 6) return;
    tick = 0;
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    jobject activity = (jobject)SDL_AndroidGetActivity();
    if (!env || !activity) return;
    jclass cls = (*env)->GetObjectClass(env, activity);
    if (!cls) return;
    jmethodID method = (*env)->GetMethodID(env, cls, "updateCompanionState", "(IIIIIIIIII)V");
    if (method) {
        int lives = 0, stage = -1, world = -1;
        int dual_screen = settings.launcher_dual_screen &&
            (game == GAME_SML || game == GAME_SML2);
        if (frame && (game == GAME_SML || game == GAME_SML2)) {
            uint8_t bcd = frame->p1_lives;
            int tens = (bcd >> 4) & 0x0F, ones = bcd & 0x0F;
            lives = (tens <= 9 && ones <= 9) ? tens * 10 + ones : bcd;
            if (frame->level_id != 0xFF) stage = frame->level_id;
            if (game == GAME_SML && frame->level_bank != 0xFF) world = frame->level_bank;
        }
        (void)(*env)->CallVoidMethod(env, activity, method,
            (jint)game, (jint)lives, (jint)stage, (jint)world,
            (jint)paused, (jint)settings.g[game].state_slot,
            (jint)settings.g[game].palette, (jint)settings.flt.preset,
            (jint)(frame ? frame->reserve_item : 0), (jint)dual_screen);
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);

        /* Send save-state paths, not pixel buffers. Java caches thumbnails and
         * decodes again only after a state file or thumbnail changes. */
        static unsigned slot_refresh_tick;
        static int last_slot_game = -1, last_selected_slot = -1;
        int selected_slot = settings.g[game].state_slot;
        if (dual_screen) slot_refresh_tick++;
        int publish_slots = dual_screen &&
            (companion_slots_dirty || game != last_slot_game ||
             selected_slot != last_selected_slot || slot_refresh_tick >= 20);
        if (publish_slots) {
            slot_refresh_tick = 0;
            jmethodID slots_method = (*env)->GetMethodID(
                env, cls, "updateCompanionSaveSlots",
                "(II[Ljava/lang/String;[Ljava/lang/String;)V");
            if (slots_method) {
                jclass string_class = (*env)->FindClass(env, "java/lang/String");
                jobjectArray state_paths = string_class
                    ? (*env)->NewObjectArray(env, 10, string_class, NULL) : NULL;
                jobjectArray thumb_paths = string_class
                    ? (*env)->NewObjectArray(env, 10, string_class, NULL) : NULL;
                char game_id[128], state_dir[1200], state_file[1200], thumb_file[1200];
                launcher_game_file_id(game, game_id, sizeof game_id);
                snprintf(state_dir, sizeof state_dir, "%sstates/", settings_dir());
                (void)mkdir_u(state_dir);
                if (state_paths && thumb_paths) {
                    for (int slot_i = 0; slot_i < 10; slot_i++) {
                        snprintf(state_file, sizeof state_file, "%s%s.slot%d.pcs",
                                 state_dir, game_id, slot_i);
                        snprintf(thumb_file, sizeof thumb_file, "%s%s.slot%d.thumb.png",
                                 state_dir, game_id, slot_i);
                        jstring state_string = (*env)->NewStringUTF(env, state_file);
                        jstring thumb_string = (*env)->NewStringUTF(env, thumb_file);
                        if (state_string)
                            (*env)->SetObjectArrayElement(env, state_paths, slot_i, state_string);
                        if (thumb_string)
                            (*env)->SetObjectArrayElement(env, thumb_paths, slot_i, thumb_string);
                        if (state_string) (*env)->DeleteLocalRef(env, state_string);
                        if (thumb_string) (*env)->DeleteLocalRef(env, thumb_string);
                    }
                    (void)(*env)->CallVoidMethod(env, activity, slots_method,
                        (jint)game, (jint)selected_slot, state_paths, thumb_paths);
                    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
                    last_slot_game = game;
                    last_selected_slot = selected_slot;
                    companion_slots_dirty = 0;
                }
                if (state_paths) (*env)->DeleteLocalRef(env, state_paths);
                if (thumb_paths) (*env)->DeleteLocalRef(env, thumb_paths);
                if (string_class) (*env)->DeleteLocalRef(env, string_class);
            } else if ((*env)->ExceptionCheck(env)) {
                (*env)->ExceptionClear(env);
            }
        }
    } else if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
    }
    (*env)->DeleteLocalRef(env, cls);
}

static void companion_music_title(const char *path, char *out, size_t n)
{
    const char *base = path ? path : "";
    const char *slash1 = strrchr(base, '/');
    const char *slash2 = strrchr(base, '\\');
    if (slash1 && (!slash2 || slash1 > slash2)) base = slash1 + 1;
    else if (slash2) base = slash2 + 1;
    if (!base[0]) {
        snprintf(out, n, "PipeClean Chiptune");
        return;
    }
    size_t len = strlen(base);
    const char *dot = strrchr(base, '.');
    if (dot && dot > base) len = (size_t)(dot - base);
    if (len >= n) len = n - 1;
    size_t w = 0;
    for (size_t i = 0; i < len && w + 1 < n; i++) {
        char ch = base[i];
        if (ch == '_') ch = ' ';
        out[w++] = ch;
    }
    out[w] = 0;
    if (!w) snprintf(out, n, "Untitled track");
}

/* Publish launcher selection, recent history, play-time statistics and preview
 * to the Companion Deck at a modest rate; large preview pixels are sent only on
 * a game change or once every few seconds. */
static void companion_publish_launcher_dashboard(int selected_game)
{
    static Uint32 last_publish_tick;
    static Uint32 last_preview_tick;
    static int last_dashboard_game = -2;
    Uint32 now = SDL_GetTicks();
    if ((Uint32)(now - last_publish_tick) < 400) return;
    last_publish_tick = now;

    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    jobject activity = (jobject)SDL_AndroidGetActivity();
    if (!env || !activity) return;
    jclass cls = (*env)->GetObjectClass(env, activity);
    if (!cls) return;
    jclass string_class = (*env)->FindClass(env, "java/lang/String");
    jobjectArray game_names = string_class
        ? (*env)->NewObjectArray(env, N_GAMES, string_class, NULL) : NULL;
    jintArray recent = (*env)->NewIntArray(env, N_RECENT_PLAYED);
    jintArray seconds = (*env)->NewIntArray(env, N_GAMES);
    int layout_game = launcher_companion_layout_target_game(selected_game);
    if (layout_game < 0 || layout_game >= N_GAMES) layout_game = selected_game;
    int editor_preview = launcher_companion_layout_preview_enabled();
    jintArray layout_values = (*env)->NewIntArray(env, N_GAMES * 16);
    jobjectArray layout_paths = string_class
        ? (*env)->NewObjectArray(env, N_GAMES * 12, string_class, NULL) : NULL;
    if (layout_values) {
        jint values[N_GAMES * 16];
        for (int g = 0; g < N_GAMES; g++) {
            CompanionLayoutCfg *layout = &settings.deck_layout[g];
            int base = g * 16;
            values[base + 0] = layout->header_dx;
            values[base + 1] = layout->header_dy;
            values[base + 2] = layout->header_scale;
            values[base + 3] = layout->item_dx;
            values[base + 4] = layout->item_dy;
            values[base + 5] = layout->item_scale;
            values[base + 6] = layout->controls_dx;
            values[base + 7] = layout->controls_dy;
            values[base + 8] = layout->controls_scale;
            values[base + 9] = layout->timeline_dx;
            values[base + 10] = layout->timeline_dy;
            values[base + 11] = layout->timeline_scale;
            values[base + 12] = layout->decoration_dx;
            values[base + 13] = layout->decoration_dy;
            values[base + 14] = layout->decoration_scale;
            values[base + 15] = layout->sounds_enabled;
        }
        (*env)->SetIntArrayRegion(env, layout_values, 0, N_GAMES * 16, values);
    }
    if (layout_paths) {
        for (int g = 0; g < N_GAMES; g++) {
            CompanionLayoutCfg *layout = &settings.deck_layout[g];
            for (int i = 0; i < 12; i++) {
                jstring path = (*env)->NewStringUTF(env, companion_layout_asset_paths(layout, i));
                if (path) {
                    (*env)->SetObjectArrayElement(env, layout_paths, g * 12 + i, path);
                    (*env)->DeleteLocalRef(env, path);
                }
            }
        }
    }
    if (game_names) {
        for (int i = 0; i < N_GAMES; i++) {
            jstring name = (*env)->NewStringUTF(env, games[i].name ? games[i].name : "Game");
            if (name) {
                (*env)->SetObjectArrayElement(env, game_names, i, name);
                (*env)->DeleteLocalRef(env, name);
            }
        }
    }
    if (recent) {
        jint values[N_RECENT_PLAYED];
        for (int i = 0; i < N_RECENT_PLAYED; i++) values[i] = settings.recent_played[i];
        (*env)->SetIntArrayRegion(env, recent, 0, N_RECENT_PLAYED, values);
    }
    if (seconds) {
        jint values[N_GAMES];
        for (int i = 0; i < N_GAMES; i++) values[i] = settings.game_play_seconds[i];
        (*env)->SetIntArrayRegion(env, seconds, 0, N_GAMES, values);
    }

    int preview_w = 0, preview_h = 0;
    jintArray preview = NULL;
    int game_changed = selected_game != last_dashboard_game;
    int want_preview = game_changed || (Uint32)(now - last_preview_tick) >= 5000;
    if (want_preview) {
        const uint32_t *pixels = NULL;
        if (launcher_preview_pixels(selected_game, &pixels, &preview_w, &preview_h) &&
            pixels && preview_w > 0 && preview_h > 0 &&
            (uint64_t)preview_w * (uint64_t)preview_h <= 1048576u) {
            jsize count = (jsize)(preview_w * preview_h);
            preview = (*env)->NewIntArray(env, count);
            if (preview) (*env)->SetIntArrayRegion(env, preview, 0, count, (const jint *)pixels);
        }
        last_preview_tick = now;
    }
    jmethodID dashboard_method = (*env)->GetMethodID(
        env, cls, "updateLauncherDashboard",
        "(I[Ljava/lang/String;[I[III[II[I[Ljava/lang/String;ZZ)V");
    if (dashboard_method && game_names && recent && seconds && layout_values && layout_paths) {
        (void)(*env)->CallVoidMethod(env, activity, dashboard_method,
            (jint)selected_game, game_names, recent, seconds,
            (jint)preview_w, (jint)preview_h, preview,
            (jint)layout_game, layout_values, layout_paths,
            settings.launcher_dual_screen ? JNI_TRUE : JNI_FALSE,
            editor_preview ? JNI_TRUE : JNI_FALSE);
    }
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);

    /* Publish the song identity and a one-shot notification serial. The deck
     * shows the vinyl banner only when dual-screen mode is active and the
     * launcher is not on the Audio & Menu management page. */
    jmethodID music_method = (*env)->GetMethodID(
        env, cls, "updateCompanionMusicPlaylist",
        "(Ljava/lang/String;[Ljava/lang/String;IZIZ)V");
    if (music_method && string_class) {
        int count = settings.menu_playlist_count;
        if (count < 0) count = 0;
        if (count > MAX_MENU_TRACKS) count = MAX_MENU_TRACKS;
        jobjectArray tracks = (*env)->NewObjectArray(env, count, string_class, NULL);
        char title[512];
        companion_music_title(settings.menu_music_path, title, sizeof title);
        jstring current_title = (*env)->NewStringUTF(env, title);
        if (tracks) {
            for (int i = 0; i < count; i++) {
                char track_title[512];
                companion_music_title(settings.menu_playlist[i], track_title, sizeof track_title);
                jstring item = (*env)->NewStringUTF(env, track_title);
                if (item) {
                    (*env)->SetObjectArrayElement(env, tracks, i, item);
                    (*env)->DeleteLocalRef(env, item);
                }
            }
        }
        if (current_title && tracks) {
            int selected = settings.menu_playlist_selected;
            if (selected < 0 || selected >= count) selected = 0;
            (void)(*env)->CallVoidMethod(env, activity, music_method,
                current_title, tracks, (jint)selected,
                settings.menu_music ? JNI_TRUE : JNI_FALSE,
                (jint)launcher_music_notification_serial(),
                launcher_music_notification_allowed() ? JNI_TRUE : JNI_FALSE);
        }
        if (current_title) (*env)->DeleteLocalRef(env, current_title);
        if (tracks) (*env)->DeleteLocalRef(env, tracks);
    }
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);

    last_dashboard_game = selected_game;
    if (preview) (*env)->DeleteLocalRef(env, preview);
    if (game_names) (*env)->DeleteLocalRef(env, game_names);
    if (recent) (*env)->DeleteLocalRef(env, recent);
    if (seconds) (*env)->DeleteLocalRef(env, seconds);
    if (layout_values) (*env)->DeleteLocalRef(env, layout_values);
    if (layout_paths) (*env)->DeleteLocalRef(env, layout_paths);
    if (string_class) (*env)->DeleteLocalRef(env, string_class);
    (*env)->DeleteLocalRef(env, cls);
}

#else
static void companion_publish_state(int game, const Frame *frame, int paused)
{
    (void)game; (void)frame; (void)paused;
}
static void companion_publish_launcher_dashboard(int selected_game)
{
    (void)selected_game;
}
#endif

#if defined(__ANDROID__)
static void companion_set_display_enabled(int enabled)
{
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    jobject activity = (jobject)SDL_AndroidGetActivity();
    if (!env || !activity) return;
    jclass cls = (*env)->GetObjectClass(env, activity);
    if (!cls) return;
    jmethodID method = (*env)->GetMethodID(env, cls, "setCompanionEnabled", "(Z)V");
    if (method) {
        (*env)->CallVoidMethod(env, activity, method, enabled ? JNI_TRUE : JNI_FALSE);
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    } else if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
    }
    (*env)->DeleteLocalRef(env, cls);
}

static void companion_set_pipe_swap(int enabled)
{
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    jobject activity = (jobject)SDL_AndroidGetActivity();
    if (!env || !activity) return;
    jclass cls = (*env)->GetObjectClass(env, activity);
    if (!cls) return;
    jmethodID method = (*env)->GetMethodID(env, cls, "setCompanionPipeSwapMode", "(Z)V");
    if (method) {
        (*env)->CallVoidMethod(env, activity, method, enabled ? JNI_TRUE : JNI_FALSE);
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    } else if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
    }
    (*env)->DeleteLocalRef(env, cls);
}

static void companion_publish_game_frame(void)
{
    if (!companion_pipe_swap_active || !SDL_AtomicGet(&companion_display_available)) return;
    /* Read the filtered CPU frame, not the SDL backbuffer, to avoid a GPU readback. */
    Uint32 now = SDL_GetTicks();
    if ((Uint32)(now - companion_last_frame_tick) < 33) return;
    companion_last_frame_tick = now;

    const uint32_t *pixels = NULL;
    int w = 0, h = 0;
    if (!render_pixels(&pixels, &w, &h) || !pixels ||
        w <= 0 || h <= 0 || w > 2048 || h > 512 ||
        (size_t)w * (size_t)h > 1048576u) return;

    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    jobject activity = (jobject)SDL_AndroidGetActivity();
    if (!env || !activity) return;
    jclass cls = (*env)->GetObjectClass(env, activity);
    if (!cls) return;
    jmethodID method = (*env)->GetMethodID(env, cls, "updateCompanionGameFrame", "(II[I)V");
    if (method) {
        jsize count = (jsize)(w * h);
        jintArray frame = (*env)->NewIntArray(env, count);
        if (frame) {
            (*env)->SetIntArrayRegion(env, frame, 0, count, (const jint *)pixels);
            if (!(*env)->ExceptionCheck(env))
                (*env)->CallVoidMethod(env, activity, method, (jint)w, (jint)h, frame);
            if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
            (*env)->DeleteLocalRef(env, frame);
        } else if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionClear(env);
        }
    } else if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
    }
    (*env)->DeleteLocalRef(env, cls);
}

#else
static void companion_set_display_enabled(int enabled) { (void)enabled; }
static void companion_set_pipe_swap(int enabled) { (void)enabled; }
static void companion_publish_game_frame(void) {}
#endif

static void companion_update_pipe_swap(int game, const Frame *frame, int enabled)
{
#if defined(__ANDROID__)
    int next = companion_pipe_swap_active;
    if (!enabled || !SDL_AtomicGet(&companion_display_available) || game != GAME_SML) {
        next = 0;
        companion_pipe_swap_exit_pending = 0;
    } else if (frame) {
        const uint8_t state = frame->game_state;
        /* SML1 HRAM states: 09=down pipe, 0A=warp underground, 0C=up out;
           12..1A cover entering/playing/leaving the pipe bonus room. */
        if (state == 0x09 || state == 0x0A || state == 0x0B ||
            (state >= 0x12 && state <= 0x1A)) {
            next = 1;
            companion_pipe_swap_exit_pending = 0;
        } else if (state == 0x0C || state == 0x1B) {
            /* Keep the game on the secondary screen throughout the climb out
               or bonus-room return. Normal state (00) marks the completed return. */
            if (next) companion_pipe_swap_exit_pending = 1;
        } else if (state == 0x00 && companion_pipe_swap_exit_pending) {
            next = 0;
            companion_pipe_swap_exit_pending = 0;
        } else if (state == 0x06 || state == 0x07 ||
                   state == 0x08 || state == 0x0E || state == 0x0F || state == 0x11) {
            next = 0;
            companion_pipe_swap_exit_pending = 0;
        }
    }
    if (next != companion_pipe_swap_active) {
        companion_pipe_swap_active = next;
        companion_last_frame_tick = 0;
        companion_set_pipe_swap(next);
        dev_diag_event(next ? "ANDROID_PIPE_SCREEN_SWAP_ON" : "ANDROID_PIPE_SCREEN_SWAP_OFF");
    }
#else
    (void)game; (void)frame; (void)enabled;
    companion_pipe_swap_active = 0;
#endif
}

static void companion_reset_pipe_swap(void)
{
    companion_pipe_swap_active = 0;
    companion_pipe_swap_exit_pending = 0;
    companion_last_frame_tick = 0;
    companion_set_pipe_swap(0);
}

static SDL_FingerID launcher_touch_finger;
static int launcher_touch_active, launcher_touch_pointer;

static void launcher_touch_event(const SDL_Event *e)
{
    if (!e) return;
    if (e->type == SDL_FINGERDOWN && !launcher_touch_active) {
        launcher_touch_active = 1;
        launcher_touch_pointer = 1;
        launcher_touch_finger = e->tfinger.fingerId;
        ui_touch_to_dp(e->tfinger.x, e->tfinger.y, &ui_mouse.x, &ui_mouse.y);
        ui_mouse.inside = 1;
        ui_mouse_button(1);
    } else if ((e->type == SDL_FINGERMOTION || e->type == SDL_FINGERUP) &&
               launcher_touch_active && e->tfinger.fingerId == launcher_touch_finger) {
        launcher_touch_pointer = 1;
        ui_touch_to_dp(e->tfinger.x, e->tfinger.y, &ui_mouse.x, &ui_mouse.y);
        ui_mouse.inside = 1;
        if (e->type == SDL_FINGERUP) {
            ui_mouse_button(0);
            launcher_touch_active = 0;
        }
    } else if (e->type == SDL_MOUSEMOTION || e->type == SDL_MOUSEBUTTONDOWN ||
               e->type == SDL_MOUSEBUTTONUP) {
        launcher_touch_pointer = 0;
    }
}

static void sfx(int w) { audio_sfx(w); }

static char game_notice_text[160];
static float game_notice_t;
static int load_state_request_app[N_GAMES];

static void game_notice(const char *s)
{
    snprintf(game_notice_text, sizeof game_notice_text, "%s", s ? s : "");
    game_notice_t = 2.2f;
}

static void game_notice_draw(float dt)
{
    if (game_notice_t <= 0 || !game_notice_text[0]) return;
    game_notice_t -= dt;
    float a = game_notice_t < 0.35f ? game_notice_t / 0.35f : 1.0f;
    float w = ui_text_w(F_BOLD, 13, game_notice_text) + 34.0f;
    float x = UI_W * 0.5f - w * 0.5f, y = UI_H - 64.0f;
    ui_shadow(x, y, w, 34, 10, 6, RGBA(0, 0, 0, (int)(100 * a)));
    ui_rrect(x, y, w, 34, 10, RGBA(36, 42, 60, (int)(245 * a)));
    ui_text_c(F_BOLD, 13, UI_W * 0.5f, y + 8, RGBA(245, 247, 252, (int)(255 * a)), game_notice_text);
}

typedef struct {
    float x, y, w, h;
    uint8_t buttons, dpad;
    const char *label;
} TouchRegion;

typedef struct {
    SDL_FingerID id;
    int active;
    int region;
} TouchFinger;

static TouchFinger touch_fingers[10];

/* Android's smaller landscape displays need larger touch targets than the
 * desktop overlay. Keep each hit region identical to its drawn button. */
#if defined(__ANDROID__)
static const TouchRegion touch_regions[] = {
    {80, UI_H - 230, 60, 60, 0, 0x04, "UP"},
    {80, UI_H - 110, 60, 60, 0, 0x08, "DN"},
    {20, UI_H - 170, 60, 60, 0, 0x02, "LT"},
    {140, UI_H - 170, 60, 60, 0, 0x01, "RT"},
    {UI_W - 148, UI_H - 218, 64, 64, 0x01, 0, "A"},
    {UI_W - 230, UI_H - 144, 64, 64, 0x02, 0, "B"},
    {UI_W - 340, UI_H - 66, 70, 50, 0x04, 0, "SEL"},
    {UI_W - 260, UI_H - 66, 80, 50, 0x08, 0, "START"}
};
#else
static const TouchRegion touch_regions[] = {
    {74, UI_H - 206, 46, 46, 0, 0x04, "UP"},
    {74, UI_H - 110, 46, 46, 0, 0x08, "DN"},
    {26, UI_H - 158, 46, 46, 0, 0x02, "LT"},
    {122, UI_H - 158, 46, 46, 0, 0x01, "RT"},
    {UI_W - 130, UI_H - 188, 56, 56, 0x01, 0, "A"},
    {UI_W - 205, UI_H - 131, 56, 56, 0x02, 0, "B"},
    {UI_W - 315, UI_H - 72, 45, 34, 0x04, 0, "SEL"},
    {UI_W - 258, UI_H - 72, 54, 34, 0x08, 0, "START"}
};
#endif

static void touch_reset_all(void)
{
    memset(touch_fingers, 0, sizeof touch_fingers);
    for (int g = 0; g < N_GAMES; g++)
        for (int player = 0; player < MAX_MP_PLAYERS; player++)
            pad_set_touch_input(g, player, 0, 0);
}

static int touch_overlay_visible(void)
{
#if defined(__ANDROID__)
    /* Handheld Android devices such as the AYN Thor have physical controls.
     * Keep the game view clean; the secondary display is reserved for the
     * companion status / utility deck rather than duplicate virtual buttons. */
    return 0;
#else
    if (!settings.touch_controls) return 0;
    return SDL_GetNumTouchDevices() > 0;
#endif
}

static int touch_finger_slot(SDL_FingerID id)
{
    for (int i = 0; i < (int)(sizeof touch_fingers / sizeof touch_fingers[0]); i++)
        if (touch_fingers[i].active && touch_fingers[i].id == id) return i;
    return -1;
}

static int touch_region_at(float x, float y)
{
    for (int i = 0; i < (int)(sizeof touch_regions / sizeof touch_regions[0]); i++) {
        const TouchRegion *r = &touch_regions[i];
        if (x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h)
            return i;
    }
    return -1;
}

static void touch_handle_event(const SDL_Event *e)
{
#if defined(__ANDROID__)
    /* Do not let invisible virtual-button hit regions intercept touch on
     * Android handhelds with physical controls. */
    (void)e;
    return;
#endif
    if (!e) return;

    /* Android can suspend or defocus SDL without delivering FINGERUP for
       every held contact. Release all virtual buttons on those transitions. */
    if ((e->type == SDL_WINDOWEVENT &&
         (e->window.event == SDL_WINDOWEVENT_FOCUS_LOST ||
          e->window.event == SDL_WINDOWEVENT_MINIMIZED)) ||
        e->type == SDL_APP_WILLENTERBACKGROUND ||
        e->type == SDL_APP_DIDENTERBACKGROUND ||
        e->type == SDL_APP_TERMINATING) {
        touch_reset_all();
        return;
    }

    if (e->type != SDL_FINGERDOWN && e->type != SDL_FINGERMOTION &&
        e->type != SDL_FINGERUP) return;
    if (!settings.touch_controls) {
        touch_reset_all();
        return;
    }
    SDL_FingerID id = e->tfinger.fingerId;
    int slot = touch_finger_slot(id);
    if (e->type == SDL_FINGERUP) {
        if (slot >= 0) {
            touch_fingers[slot].active = 0;
            touch_fingers[slot].region = -1;
        }
        return;
    }
    if (slot < 0 && e->type == SDL_FINGERDOWN) {
        for (int i = 0; i < (int)(sizeof touch_fingers / sizeof touch_fingers[0]); i++) {
            if (!touch_fingers[i].active) { slot = i; break; }
        }
        if (slot >= 0) {
            touch_fingers[slot].id = id;
            touch_fingers[slot].active = 1;
            touch_fingers[slot].region = -1;
        }
    }
    if (slot < 0) return;
    float x, y;
    ui_touch_to_dp(e->tfinger.x, e->tfinger.y, &x, &y);
    touch_fingers[slot].region = touch_region_at(x, y);
}

static int touch_region_held(int region)
{
    for (int i = 0; i < (int)(sizeof touch_fingers / sizeof touch_fingers[0]); i++)
        if (touch_fingers[i].active && touch_fingers[i].region == region) return 1;
    return 0;
}

static void touch_apply_inputs(int game, int paused)
{
    uint8_t buttons = 0, dpad = 0;
    if (settings.touch_controls && !paused) {
        for (int i = 0; i < (int)(sizeof touch_fingers / sizeof touch_fingers[0]); i++) {
            if (!touch_fingers[i].active || touch_fingers[i].region < 0 ||
                touch_fingers[i].region >= (int)(sizeof touch_regions / sizeof touch_regions[0])) continue;
            buttons |= touch_regions[touch_fingers[i].region].buttons;
            dpad |= touch_regions[touch_fingers[i].region].dpad;
        }
    }
    pad_set_touch_input(game, 0, buttons, dpad);
}

static void touch_reset_game(int game)
{
    touch_reset_all();
    if (game >= 0 && game < N_GAMES)
        pad_set_touch_input(game, 0, 0, 0);
}

static void touch_draw_overlay(int paused)
{
    if (!touch_overlay_visible() || paused) return;
    for (int i = 0; i < (int)(sizeof touch_regions / sizeof touch_regions[0]); i++) {
        const TouchRegion *r = &touch_regions[i];
        int held = touch_region_held(i);
        uint32_t fill = held ? RGBA(76, 141, 255, 190) : RGBA(14, 18, 29, 112);
        uint32_t edge = held ? RGBA(225, 237, 255, 235) : RGBA(205, 216, 240, 125);
        ui_rrect(r->x, r->y, r->w, r->h, 10, fill);
        ui_stroke(r->x, r->y, r->w, r->h, 10, 1, edge);
        ui_text_c(F_BOLD, 11, r->x + r->w * 0.5f,
                  r->y + (r->h - ui_line_h(11)) * 0.5f,
                  RGBA(255, 255, 255, held ? 255 : 220), r->label);
    }
}

static int app_key_down(int key)
{
    if (!key) return 0;
    SDL_Scancode sc = SDL_GetScancodeFromKey((SDL_Keycode)key);
    if (sc == SDL_SCANCODE_UNKNOWN) return 0;
    const Uint8 *ks = SDL_GetKeyboardState(NULL);
    return ks[sc] != 0;
}

static int state_file_exists(const char *path)
{
    FILE *f = path && path[0] ? fopen(path, "rb") : NULL;
    if (!f) return 0;
    fclose(f);
    return 1;
}

static int thumb_pending_game = -1;
static int thumb_pending_slot = -1;
static int thumb_pending_delay = 0;

static void state_thumb_path(int g, int slot, char *out, size_t n)
{
    char dir[1200], id[128];
    snprintf(dir, sizeof dir, "%sstates/", settings_dir());
    mkdir_u(dir);
    launcher_game_file_id(g, id, sizeof id);
    snprintf(out, n, "%s%s.slot%d.thumb.png", dir, id, slot);
}

static void queue_state_thumbnail(int g, int slot)
{
    thumb_pending_game = g;
    thumb_pending_slot = slot;
    /* The first rendered frame after saving can contain the save-state flash. */
    thumb_pending_delay = 1;
}

static void capture_pending_state_thumbnail(void)
{
    if (thumb_pending_game < 0 || thumb_pending_slot < 0 || !ren) return;
    if (thumb_pending_delay > 0) {
        thumb_pending_delay--;
        return;
    }
    int W, H;
    if (SDL_GetRendererOutputSize(ren, &W, &H) != 0 || W <= 0 || H <= 0) return;

    SDL_Surface *src = SDL_CreateRGBSurfaceWithFormat(0, W, H, 32, SDL_PIXELFORMAT_ARGB8888);
    SDL_Surface *dst = SDL_CreateRGBSurfaceWithFormat(0, 320, 180, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!src || !dst) {
        if (src) SDL_FreeSurface(src);
        if (dst) SDL_FreeSurface(dst);
        return;
    }
    if (SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_ARGB8888, src->pixels, src->pitch) == 0) {
        SDL_FillRect(dst, NULL, SDL_MapRGB(dst->format, 0, 0, 0));
        float scale = fminf(320.0f / (float)W, 180.0f / (float)H);
        int dw = (int)(W * scale + 0.5f);
        int dh = (int)(H * scale + 0.5f);
        SDL_Rect dr = {(320 - dw) / 2, (180 - dh) / 2, dw, dh};
        SDL_BlitScaled(src, NULL, dst, &dr);
        char path[1200];
        state_thumb_path(thumb_pending_game, thumb_pending_slot, path, sizeof path);
        if (IMG_SavePNG(dst, path) == 0) companion_slots_dirty = 1;
    }
    SDL_FreeSurface(src);
    SDL_FreeSurface(dst);
    thumb_pending_game = -1;
    thumb_pending_slot = -1;
    thumb_pending_delay = 0;
}

static void state_path(int g, int slot, char *out, size_t n)
{
    char dir[1200], id[128];
    snprintf(dir, sizeof dir, "%sstates/", settings_dir());
    mkdir_u(dir);
    launcher_game_file_id(g, id, sizeof id);
    snprintf(out, n, "%s%s.slot%d.pcs", dir, id, slot);
}

static void suspend_path(int g, char *out, size_t n)
{
    char dir[1200], id[128];
    snprintf(dir, sizeof dir, "%sstates/", settings_dir());
    mkdir_u(dir);
    launcher_game_file_id(g, id, sizeof id);
    snprintf(out, n, "%s%s.suspend.pcs", dir, id);
}

static int game_state_save(int g, int paused)
{
    char path[1200];
    state_path(g, settings.g[g].state_slot, path, sizeof path);
    if (emu_rewind_available()) emu_rewind_end();
    if (emu_state_save_file(path, 1) != 0) {
        game_notice("Couldn't save state.");
        return -1;
    }
    queue_state_thumbnail(g, settings.g[g].state_slot);
    companion_slots_dirty = 1;
    emu_set_paused(paused);
    char msg[96];
    snprintf(msg, sizeof msg, "State saved — Slot %d", settings.g[g].state_slot + 1);
    game_notice(msg);
    return 0;
}

static int game_state_load(int g, int paused)
{
    char path[1200];
    state_path(g, settings.g[g].state_slot, path, sizeof path);
    if (!state_file_exists(path)) {
        char msg[96];
        snprintf(msg, sizeof msg, "No saved state in Slot %d", settings.g[g].state_slot + 1);
        game_notice(msg);
        return -1;
    }
    emu_rewind_end();
    if (emu_state_load_file(path, 1) != 0) {
        game_notice("Couldn't load state.");
        return -1;
    }
    /* A load can jump across rooms/scenes, so discard filter history from the old picture. */
    render_reset();
    emu_set_paused(paused);
    char msg[96];
    snprintf(msg, sizeof msg, "State loaded — Slot %d", settings.g[g].state_slot + 1);
    game_notice(msg);
    return 0;
}

static int game_suspend(int g)
{
    char path[1200];
    suspend_path(g, path, sizeof path);
    emu_rewind_end();
    if (emu_state_save_file(path, 0) != 0) {
        game_notice("Couldn't suspend the game.");
        return -1;
    }
    game_notice("Game suspended.");
    return 0;
}

static int game_shared_state_save(int g)
{
    int slot = netplay_shared_slot_active();
    char path[1200], marker[1200];
    if (slot < 0 || slot >= NETPLAY_SHARED_SLOT_COUNT) {
        game_notice("Choose a shared save slot in the Netplay lobby first.");
        return -1;
    }
    launcher_shared_slot_paths(g, slot, path, sizeof path, marker, sizeof marker);
    if (!path[0] || !marker[0]) {
        game_notice("Couldn't prepare the shared save slot.");
        return -1;
    }
    settings.g[g].state_slot = slot;
    netplay_shared_slot_mark_unsynced(slot);
    if (!netplay_is_host()) {
        queue_state_thumbnail(g, slot);
        game_notice("Shared save requested — waiting for the host to sync it.");
        return 0;
    }
    if (emu_state_save_file(path, 0) != 0) {
        game_notice("Couldn't create the shared save.");
        return -1;
    }
    queue_state_thumbnail(g, slot);
    if (!netplay_shared_save_send_file(slot, path)) {
        game_notice("Couldn't send the shared save to the other player.");
        return -1;
    }
    char msg[96];
    snprintf(msg, sizeof msg, "Syncing shared save — Slot %d", slot + 1);
    game_notice(msg);
    return 0;
}

static int game_shared_state_load(int g, int slot)
{
    char path[1200], marker[1200];
    int left = 0, right = 0;
    if (slot < 0 || slot >= NETPLAY_SHARED_SLOT_COUNT ||
        !netplay_shared_slot_is_synced(slot)) {
        game_notice("That shared slot isn't synchronized on both players.");
        return -1;
    }
    launcher_shared_slot_paths(g, slot, path, sizeof path, marker, sizeof marker);
    if (!state_file_exists(path) || !state_file_exists(marker)) {
        game_notice("The shared save file is missing on this device.");
        return -1;
    }
    if (emu_rewind_available()) emu_rewind_end();
    if (emu_state_load_file(path, 0) != 0) {
        game_notice("Couldn't load the shared save. The session was not changed.");
        return -1;
    }
    /* Core state is shared, but each player's preferred PPU output width is local. */
    wide_dims(g, settings.g[g].wide, &left, &right);
    ppu_set_wide(left, right, games[g].hud_lines, games[g].hud_window, games[g].wide_gate);
    render_reset();
    settings.g[g].state_slot = slot;
    char msg[96];
    snprintf(msg, sizeof msg, "Loaded shared state — Slot %d", slot + 1);
    game_notice(msg);
    dev_diag_event("ONLINE_SHARED_STATE_LOADED");
    return 0;
}

static int game_action_key_match(int g, int action, const SDL_Event *e)
{
    if (!e || e->type != SDL_KEYDOWN || e->key.repeat || action < 0 || action >= N_ACTION) return 0;
    return settings.g[g].action_key[action] != 0 &&
           e->key.keysym.sym == settings.g[g].action_key[action];
}

static int app_handle_action_event(int g, const SDL_Event *e, int *paused, int *quit)
{
    if (!e || g < 0 || g >= N_GAMES) return 0;
    for (int a = 0; a < N_ACTION; a++) {
        if (!game_action_key_match(g, a, e) && !pad_binding_event(g, a, e)) continue;
        switch (a) {
        case ACT_SAVE_STATE:
            game_state_save(g, paused ? *paused : 0);
            break;
        case ACT_LOAD_STATE:
            if (g >= 0 && g < N_GAMES) load_state_request_app[g] = 1;
            break;
        case ACT_REWIND:
            break; /* held state is handled every frame below */
        case ACT_SUSPEND:
            if (game_suspend(g) == 0 && quit) *quit = 1;
            break;
        case ACT_NEXT_SLOT:
            settings.g[g].state_slot = (settings.g[g].state_slot + 1) % 10;
            {
                char msg[64];
                snprintf(msg, sizeof msg, "State Slot %d", settings.g[g].state_slot + 1);
                game_notice(msg);
            }
            break;
        }
        return a + 1;
    }
    return 0;
}

static int app_rewind_held(int g)
{
    if (g < 0 || g >= N_GAMES) return 0;
    return app_key_down(settings.g[g].action_key[ACT_REWIND]) ||
           pad_binding_down(g, ACT_REWIND);
}

static int companion_handle_event(int g, const SDL_Event *e, int *paused, int *quit)
{
    if (!e || e->type != SDL_USEREVENT || e->user.code != PIPECLEAN_COMPANION_EVENT_CODE)
        return 0;
    int action = (int)(intptr_t)e->user.data1;
    int pressed = (int)(intptr_t)e->user.data2;
    if (g < 0 || g >= N_GAMES) return 1;
    if (action == PIPECLEAN_COMPANION_REWIND) {
        companion_rewind_held = pressed != 0;
        return 1;
    }
    if (!pressed) return 1;
    if (action >= PIPECLEAN_COMPANION_SLOT_SELECT_BASE &&
        action < PIPECLEAN_COMPANION_SLOT_SELECT_BASE + 10) {
        settings.g[g].state_slot = action - PIPECLEAN_COMPANION_SLOT_SELECT_BASE;
        companion_slots_dirty = 1;
        settings_save();
        game_notice("Save slot selected.");
        return 1;
    }
    switch (action) {
    case PIPECLEAN_COMPANION_SAVE:
        if (netplay_is_connected()) game_notice("Save from the second screen is disabled during online co-op.");
        else (void)game_state_save(g, paused ? *paused : 0);
        break;
    case PIPECLEAN_COMPANION_LOAD:
        if (netplay_is_connected()) game_notice("Load from the second screen is disabled during online co-op.");
        else load_state_request_app[g] = 1;
        break;
    case PIPECLEAN_COMPANION_PAUSE:
        if (netplay_is_connected()) game_notice("Pause is disabled during online co-op.");
        else if (paused) {
            *paused = !*paused;
            emu_set_paused(*paused);
            game_notice(*paused ? "Game paused." : "Game resumed.");
        }
        break;
    case PIPECLEAN_COMPANION_PALETTE:
        settings.g[g].palette = (settings.g[g].palette + 1) % n_palettes;
        render_reset(); settings_save();
        break;
    case PIPECLEAN_COMPANION_FILTER:
        filter_preset(&settings.flt, (settings.flt.preset + 1) % n_filter_presets);
        render_reset(); settings_save();
        break;
    case PIPECLEAN_COMPANION_SLOT_PREV:
        settings.g[g].state_slot = (settings.g[g].state_slot + 9) % 10;
        companion_slots_dirty = 1;
        settings_save(); game_notice("Previous save slot selected.");
        break;
    case PIPECLEAN_COMPANION_SLOT_NEXT:
        settings.g[g].state_slot = (settings.g[g].state_slot + 1) % 10;
        companion_slots_dirty = 1;
        settings_save(); game_notice("Next save slot selected.");
        break;
    case PIPECLEAN_COMPANION_ITEM_USE:
        /* The emulation thread applies the stored power-up using the game's
         * own item state/object mechanics. Empty slots safely do nothing. */
        emu_reserve_item_use();
        break;
    case PIPECLEAN_COMPANION_CLOSE_GAME:
        if (quit) *quit = 1;
        break;
    default: break;
    }
    (void)quit;
    return 1;
}

enum {
    DS_MENU_SAVE, DS_MENU_LOAD, DS_MENU_SLOT_PLUS, DS_MENU_REWIND,
    DS_MENU_SLOT_MINUS, DS_MENU_SUSPEND, DS_MENU_PAUSE,
    DS_MENU_COUNT
};

typedef struct {
    int open;
    int touch_active;
    float touch_x, touch_y;
    int selected;
} DsStateMenu;

static DsStateMenu ds_menu;

static int ds_menu_select(float x, float y)
{
    float dx = x - 0.5f, dy = y - 0.5f;
    if (dx * dx + dy * dy < 0.12f * 0.12f) return -1;
    float a = atan2f(dy, dx);
    float sector = (float)(2.0 * 3.14159265358979323846 / DS_MENU_COUNT);
    int s = (int)floorf(((float)3.14159265358979323846f / 8.0f - a) / sector + 0.5f);
    s %= DS_MENU_COUNT;
    if (s < 0) s += DS_MENU_COUNT;
    return s;
}

static void ds_menu_open(void)
{
    ds_menu.open = 1;
    ds_menu.touch_active = 0;
    ds_menu.selected = -1;
}

static void ds_menu_close(void)
{
    ds_menu.open = 0;
    ds_menu.touch_active = 0;
    ds_menu.selected = -1;
}

static int ds_menu_event(int g, const SDL_Event *e, int *paused, int *quit)
{
    if (!e || g < 0 || g >= N_GAMES) return 0;
    if (e->type == SDL_CONTROLLERBUTTONDOWN &&
        e->cbutton.button == SDL_CONTROLLER_BUTTON_TOUCHPAD &&
        pad_is_selected_dualsense_instance(g, e->cbutton.which)) {
        if (ds_menu.open) {
            emu_rewind_end();
            ds_menu_close();
        } else {
            ds_menu_open();
        }
        return 1;
    }

    float x, y;
    int kind;
    if (!pad_touchpad_event(e, &x, &y, &kind)) return 0;
    /* Touchpad events may originate from multiple DualSense controllers; only P1's selected device may control the menu. */
    if (!pad_is_selected_dualsense_instance(g, e->ctouchpad.which)) return 0;

    if (kind == 0) {
        if (!ds_menu.open) ds_menu_open();
        ds_menu.touch_active = 1;
        ds_menu.touch_x = x; ds_menu.touch_y = y;
        ds_menu.selected = ds_menu_select(x, y);
    } else if (kind == 1 && ds_menu.open) {
        int old = ds_menu.selected;
        ds_menu.touch_x = x; ds_menu.touch_y = y;
        ds_menu.selected = ds_menu_select(x, y);
        if (ds_menu.selected == DS_MENU_REWIND && old != DS_MENU_REWIND) render_reset();
        if (old == DS_MENU_REWIND && ds_menu.selected != DS_MENU_REWIND) {
            emu_rewind_end();
            render_reset();
        }
    } else if (kind == 2 && ds_menu.open) {
        ds_menu.touch_active = 0;
        ds_menu.touch_x = x; ds_menu.touch_y = y;
        ds_menu.selected = ds_menu_select(x, y);
        int a = ds_menu.selected;
        if (a == DS_MENU_SAVE) game_state_save(g, paused ? *paused : 0);
        else if (a == DS_MENU_LOAD) game_state_load(g, paused ? *paused : 0);
        else if (a == DS_MENU_SLOT_PLUS) { settings.g[g].state_slot = (settings.g[g].state_slot + 1) % 10; game_notice("Next state slot"); }
        else if (a == DS_MENU_SLOT_MINUS) { settings.g[g].state_slot = (settings.g[g].state_slot + 9) % 10; game_notice("Previous state slot"); }
        else if (a == DS_MENU_REWIND) emu_rewind_end();
        else if (a == DS_MENU_SUSPEND) { if (game_suspend(g) == 0 && quit) *quit = 1; }
        else if (a == DS_MENU_PAUSE) { *paused = !*paused; emu_set_paused(*paused); }
        if (a != DS_MENU_REWIND) emu_rewind_end();
        ds_menu_close();
    }
    return 1;
}

static uint32_t ds_mixc(uint32_t a, uint32_t b, float t)
{
    return RGBA((int)(((a >> 24) & 255) * (1 - t) + ((b >> 24) & 255) * t),
                (int)(((a >> 16) & 255) * (1 - t) + ((b >> 16) & 255) * t),
                (int)(((a >> 8) & 255) * (1 - t) + ((b >> 8) & 255) * t),
                (int)((a & 255) * (1 - t) + (b & 255) * t));
}

static void ds_menu_draw(int g)
{
    if (!ds_menu.open || g < 0 || g >= N_GAMES) return;
    static const char *labels[DS_MENU_COUNT] = {"Save", "Load", "Slot +", "Rewind", "Slot -", "Suspend", "Pause"};
    static const char *desc[DS_MENU_COUNT] = {
        "save state", "load state", "next slot", "hold to rewind",
        "previous slot", "save & return", "pause game"
    };
    float cx = UI_W * 0.5f, cy = UI_H * 0.5f;
    ui_shadow(cx - 270, cy - 220, 540, 440, 20, 12, RGBA(0, 0, 0, 135));
    ui_rrect(cx - 270, cy - 220, 540, 440, 20, RGBA(16, 19, 28, 244));
    ui_text_c(F_BOLD, 22, cx, cy - 190, C_TEXT, "DualSense • PipeClean");
    char slot[64];
    snprintf(slot, sizeof slot, "State Slot %d", settings.g[g].state_slot + 1);
    ui_text_c(F_REG, 12, cx, cy - 162, C_MUTED, slot);
    for (int i = 0; i < DS_MENU_COUNT; i++) {
        float a = -(3.14159265358979323846f * 2.0f * i / DS_MENU_COUNT) + 3.14159265358979323846f / 8.0f;
        float x = cx + cosf(a) * 180.0f, y = cy + sinf(a) * 140.0f;
        int hot = i == ds_menu.selected;
        ui_rrect(x - 68, y - 27, 136, 54, 12, hot ? ds_mixc(C_BTN_H, HEX(ui_accent), 0.28f) : C_BTN);
        if (hot) ui_stroke(x - 68, y - 27, 136, 54, 12, 2, HEX(ui_accent));
        ui_text_c(F_BOLD, 13, x, y - 15, hot ? C_TEXT : C_MUTED, labels[i]);
        ui_text_c(F_REG, 9, x, y + 4, C_DIM, desc[i]);
    }
    ui_text_c(F_REG, 11, cx, cy + 188, C_DIM, "Touch • slide to choose • release to confirm");
}

static void out_size(int *w, int *h)
{
    if (SDL_GetRendererOutputSize(ren, w, h) != 0 || *w <= 0 || *h <= 0) SDL_GetWindowSize(win, w, h);
}

static void mouse_sync(void)
{
    int mx, my, ww, wh;
    SDL_GetMouseState(&mx, &my);
    SDL_GetWindowSize(win, &ww, &wh);
    ui_set_mouse(mx, my, ww, wh);
}

static int launcher_applied_scale = -1;
static int launcher_applied_window_size = -1;
static int launcher_applied_fullscreen = -1;

static void launcher_display_cache_reset(void)
{
    launcher_applied_scale = -1;
    launcher_applied_window_size = -1;
    launcher_applied_fullscreen = -1;
}

static void apply_launcher_display_preferences(void)
{
    if (launcher_applied_scale != settings.launcher_ui_scale) {
        ui_set_scale_preference(settings.launcher_ui_scale);
        launcher_applied_scale = settings.launcher_ui_scale;
    }
#if !defined(__ANDROID__)
    int width = 1100, height = 720;
    if (settings.launcher_window_size == 0) { width = 900; height = 580; }
    else if (settings.launcher_window_size == 2) { width = 1440; height = 900; }

    int fullscreen_changed = launcher_applied_fullscreen != settings.launcher_fullscreen;
    int size_changed = launcher_applied_window_size != settings.launcher_window_size;
    if (fullscreen_changed) {
        SDL_SetWindowFullscreen(win, settings.launcher_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
        launcher_applied_fullscreen = settings.launcher_fullscreen;
        if (!settings.launcher_fullscreen) size_changed = 1;
    }
    if (size_changed) {
        if (!settings.launcher_fullscreen) {
            win_w = width;
            win_h = height;
            SDL_SetWindowSize(win, win_w, win_h);
            SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        }
        launcher_applied_window_size = settings.launcher_window_size;
    }
#endif
}

static void set_game_window(int g)
{
    const GameCfg *c = &settings.g[g];
    if (c->size == SIZE_FULLSCREEN) { SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN_DESKTOP); return; }
    SDL_SetWindowFullscreen(win, 0);
    int hh = c->size == SIZE_SMALL ? 432 : (c->size == SIZE_MEDIUM ? 576 : 720);
    float ar = c->aspect == ASPECT_4_3 ? 4.0f / 3 : (c->aspect == ASPECT_16_9 ? 16.0f / 9 : ppu_w / 144.0f);
    SDL_SetWindowSize(win, (int)(hh * ar), hh);
    SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
}

static void draw_bg_cover(float dim)
{
    int bw, bh, W, H;
    SDL_Texture *t = bg_texture(&bw, &bh);
    out_size(&W, &H);
    if (!t) return;
    float s = fmaxf((float)W / bw, (float)H / bh);
    SDL_Rect src = {(int)((bw - W / s) * 0.5f), (int)((bh - H / s) * 0.5f), (int)(W / s), (int)(H / s)};
    SDL_RenderCopy(ren, t, &src, NULL);
    if (dim > 0) {
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(ren, 0, 0, 0, (Uint8)(dim * 2.55f));
        SDL_RenderFillRect(ren, NULL);
    }
}

static int sml1_oam_anchor(const Frame *f, const uint8_t oam[16], float *gx, float *gy)
{
    if (!f || !oam || !gx || !gy || f->w <= 0) return 0;
    int L = f->xoff;
    int sprite_neg = L > 8 ? 256 - (L - 8) : 256;
    int minx = 10000, maxx = -10000, miny = 10000;
    int count = 0;
    for (int i = 0; i < 4; i++) {
        const uint8_t *s = &oam[i * 4];
        if (!s[0]) continue;
        int x = s[1];
        if (x >= sprite_neg) x -= 256;
        x = x - 8 + L;
        int y = s[0] - 16;
        if (x < minx) minx = x;
        if (x + 8 > maxx) maxx = x + 8;
        if (y < miny) miny = y;
        count++;
    }
    if (!count) return 0;
    *gx = (minx + maxx) * 0.5f;
    *gy = (float)miny;
    return 1;
}

static void sml1_draw_player_tag(const Frame *f, const uint8_t oam[16], const SDL_Rect *r,
                                 const char *label, uint32_t color)
{
    float gx, gy;
    if (!sml1_oam_anchor(f, oam, &gx, &gy) || !r || r->w <= 0 || r->h <= 0) return;

    float sx = r->x + gx / (float)f->w * r->w;
    float sy = r->y + (gy - 2.0f) / (float)GB_H * r->h;
    float ux = sx / ui_scale + ui_view_x0();
    float uy = sy / ui_scale + ui_view_y0();

    float tw = ui_text_w(F_BOLD, 12, label) + 12.0f;
    float tx = ux - tw * 0.5f;
    float ty = uy - 22.0f;

    ui_shadow(tx, ty, tw, 18, 6, 3, RGBA(0, 0, 0, 105));
    ui_rrect(tx, ty, tw, 18, 6, color);
    ui_text_c(F_BOLD, 12, ux, ty + 1, HEX(0xFFFFFF), label);
    ui_tri(ux - 5, ty + 16, ux + 5, ty + 16, ux, uy - 1, color);
}

static int sml1_bcd_to_int(uint8_t b)
{
    return ((b >> 4) & 0x0F) * 10 + (b & 0x0F);
}

static const char *mp_player_name(int game, int player)
{
    const GameCfg *c = &settings.g[game];
    switch (player) {
    case 0: return "Mario";
    case 1: return c->p2_name[0] ? c->p2_name : "Luigi";
    case 2: return c->p3_name[0] ? c->p3_name : "Bunzo";
    case 3: return c->p4_name[0] ? c->p4_name : "Florbo";
    default: return "Player";
    }
}

static int mp_respawn_key(const GameCfg *c, int player)
{
    if (!c) return 0;
    switch (player) {
    case 1: return c->p2_respawn_key;
    case 2: return c->p3_respawn_key;
    case 3: return c->p4_respawn_key;
    default: return 0;
    }
}

static int mp_respawn_pad(const GameCfg *c, int player)
{
    if (!c) return -1;
    switch (player) {
    case 1: return c->p2_respawn_pad;
    case 2: return c->p3_respawn_pad;
    case 3: return c->p4_respawn_pad;
    default: return -1;
    }
}

static void draw_coop_hud(int game, const Frame *f)
{
    if (!f) return;
    int count = f->mp_player_count;
    if (count < 2) count = 2;
    if (count > MAX_MP_PLAYERS) count = MAX_MP_PLAYERS;

    float vx = ui_view_x0(), vw = ui_view_w();
    const float y = 12.0f, h = 36.0f, gap = 7.0f, edge = 10.0f;
    float w = (vw - edge * 2.0f - gap * (count - 1)) / count;
    if (w > 215.0f) w = 215.0f;
    if (w < 86.0f) w = 86.0f;
    float total = count * w + (count - 1) * gap;
    float x0 = vx + (vw - total) * 0.5f;

    for (int player = 0; player < count; player++) {
        float x = x0 + player * (w + gap);
        ui_shadow(x, y, w, h, 8, 3, RGBA(0, 0, 0, 105));
        ui_rrect(x, y, w, h, 8, RGBA(22, 25, 34, 235));
        uint32_t color = player == 0 ? C_ACCENT : render_sml1_mp_player_color(player);
        char title[48], lives[16];
        snprintf(title, sizeof title, "P%d  %.20s", player + 1, mp_player_name(game, player));
        int life_count = player == 0 ? sml1_bcd_to_int(f->p1_lives) : f->mp_player_lives[player];
        snprintf(lives, sizeof lives, "x %d", life_count);
        ui_text_fit(F_BOLD, 11, x + 8, y + 4, w - 40, color, title);
        ui_text_r(F_BOLD, 12, x + w - 7, y + 4, C_TEXT, lives);
        if (player > 0 && !f->mp_player_visible[player] && life_count > 0)
            ui_text(F_REG, 9, x + 8, y + 21, HEX(0xA8B0C0), "Respawning...");
    }
}

/* Local SML1 multiplayer: Mario owns the shared world; up to three extra
 * characters each run through an isolated clone of the original SML1 logic. */
/* Local SML1 multiplayer: one shared world with two real SML1 player states. */
static int play_multiplayer(int g)
{
    touch_reset_all();
    companion_reset_pipe_swap();
    companion_rewind_held = 0;
    ds_menu_close();
    GameCfg *c = &settings.g[g];
    int online = netplay_is_connected();
    int online_display_left = 0, online_display_right = 0;
    if (online) wide_dims(g, c->wide, &online_display_left, &online_display_right);
    int player_count = online ? 2 : c->multiplayer_players;
    if (player_count < 2) player_count = c->multiplayer_players = 2;
    if (player_count > MAX_MP_PLAYERS) player_count = c->multiplayer_players = MAX_MP_PLAYERS;

    char sp[1200], save_id[128];
    launcher_game_file_id(g, save_id, sizeof save_id);
    snprintf(sp, sizeof sp, "%ssaves/", settings_dir());
    mkdir_u(sp);
    snprintf(sp, sizeof sp, "%ssaves/%s.sav", settings_dir(), save_id);
    emu_set_save_path(online ? "" : sp);

    bg_load(c->bg_path);
    texpack_load(c->tex_on ? c->tex_path : "");
    tex_collect_begin(g);
    render_reset();
    pad_set_context(g, 1);
    set_game_window(g);

    int requested_load = launcher_take_load_state(g);
    if (online) requested_load = -1;
    char suspended[1200];
    suspend_path(g, suspended, sizeof suspended);
    int has_suspend = !online && state_file_exists(suspended);
    render_set_mp_game(g);
    audio_mp_set_game(g);
    audio_menu_apply();
    int saved_player_count = c->multiplayer_players;
    if (online) c->multiplayer_players = 2;
    int mp_begin_result = emu_mp_begin();
    if (online) c->multiplayer_players = saved_player_count;
    if (mp_begin_result) {
        if (online) netplay_game_end();
        launcher_toast("Couldn't start multiplayer.");
        return 0;
    }
    int initial_shared_load = online ? netplay_shared_slot_load() : -1;
    if (online && initial_shared_load >= 0) {
        char shared_path[1200], marker_path[1200];
        launcher_shared_slot_paths(g, initial_shared_load,
                                   shared_path, sizeof shared_path,
                                   marker_path, sizeof marker_path);
        if (!netplay_shared_slot_is_synced(initial_shared_load) ||
            !state_file_exists(shared_path) || !state_file_exists(marker_path) ||
            emu_state_load_file(shared_path, 0) != 0) {
            emu_mp_end();
            netplay_game_end();
            tex_collect_save();
            pad_set_context(g, 0);
            launcher_toast("Couldn't load the shared starting save. Check both players' slots.");
            return 0;
        }
        ppu_set_wide(online_display_left, online_display_right,
                     games[g].hud_lines, games[g].hud_window, games[g].wide_gate);
        settings.g[g].state_slot = initial_shared_load;
        render_reset();
    }
    if (requested_load >= 0) {
        c->state_slot = requested_load;
        (void)game_state_load(g, 0);
    } else if (has_suspend) {
        if (emu_state_load_file(suspended, 1) == 0) remove(suspended);
    }
    apu_set_volume(settings.volume / 100.0f);
    audio_game_begin();

    Frame *f = (Frame *)calloc(1, sizeof *f);
    Frame *player_frames[MAX_MP_PLAYERS - 1] = {NULL, NULL, NULL};
    int16_t a0[4096 * 2];
    int alloc_failed = !f;
    for (int player = 1; player < player_count; player++) {
        player_frames[player - 1] = (Frame *)calloc(1, sizeof *player_frames[player - 1]);
        if (!player_frames[player - 1]) alloc_failed = 1;
    }
    if (alloc_failed) {
        free(f);
        for (int i = 0; i < MAX_MP_PLAYERS - 1; i++) free(player_frames[i]);
        audio_game_end();
        emu_mp_end();
        if (online) netplay_game_end();
        tex_collect_save();
        pad_set_context(g, 0);
        launcher_toast("Not enough memory for multiplayer.");
        return 0;
    }

    int quit = 0, paused = 0, have = 0, shot = 0;
    int rewind_held = 0;
    uint8_t buttons[MAX_MP_PLAYERS] = {0, 0, 0, 0};
    uint8_t dpad[MAX_MP_PLAYERS] = {0, 0, 0, 0};
    uint8_t online_actions = 0;
    uint64_t online_frame = 0;
    Uint64 last = SDL_GetPerformanceCounter();
    dev_diag_begin(g, player_count, win);

    while (!quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (companion_handle_event(g, &e, &paused, &quit)) continue;
            if (dev_diag_handle_event(&e)) continue;
            if (e.type == SDL_WINDOWEVENT &&
                e.window.event == SDL_WINDOWEVENT_CLOSE &&
                e.window.windowID == SDL_GetWindowID(win)) {
                quit = 2;
                continue;
            }
            touch_handle_event(&e);
            pad_event(&e);
            int ds_consumed = online ? 0 : ds_menu_event(g, &e, &paused, &quit);
            if (ds_consumed) continue;

            if (e.type == SDL_KEYDOWN && !e.key.repeat) {
                for (int player = 1; player < player_count; player++) {
                    int key = mp_respawn_key(c, player);
                    if (key && e.key.keysym.sym == key) {
                        if (online) online_actions |= NETPLAY_ACTION_RESPAWN_P2;
                        else emu_mp_request_respawn(player);
                        {
                            char diagnostic[96];
                            snprintf(diagnostic, sizeof diagnostic, "%s RESPAWN REQUESTED", mp_player_name(g, player));
                            dev_diag_event(diagnostic);
                        }
                        char msg[96];
                        snprintf(msg, sizeof msg, "%s respawn requested.", mp_player_name(g, player));
                        game_notice(msg);
                    }
                }
            }
            for (int player = 1; player < player_count; player++) {
                int binding = mp_respawn_pad(c, player);
                int device = c->pad_device[online ? 0 : player];
                if (binding >= 0 && device >= 0 && device < pad_count() &&
                    pad_capture(device, &e) == binding) {
                    if (online) online_actions |= NETPLAY_ACTION_RESPAWN_P2;
                    else emu_mp_request_respawn(player);
                    {
                        char diagnostic[96];
                        snprintf(diagnostic, sizeof diagnostic, "%s RESPAWN REQUESTED", mp_player_name(g, player));
                        dev_diag_event(diagnostic);
                    }
                    char msg[96];
                    snprintf(msg, sizeof msg, "%s respawn requested.", mp_player_name(g, player));
                    game_notice(msg);
                }
            }

            int action = 0;
            if (online) {
                for (int a = ACT_SAVE_STATE; a <= ACT_LOAD_STATE; a++) {
                    if (!game_action_key_match(g, a, &e) && !pad_binding_event(g, a, &e)) continue;
                    online_actions |= (uint8_t)(a == ACT_SAVE_STATE
                        ? NETPLAY_ACTION_SAVE_STATE : NETPLAY_ACTION_LOAD_STATE);
                    action = a + 1;
                    game_notice(a == ACT_SAVE_STATE
                        ? "Shared save requested…" : "Shared state load requested…");
                    break;
                }
            } else {
                action = app_handle_action_event(g, &e, &paused, &quit);
            }
            if (e.type == SDL_QUIT) quit = 2;
            else if (e.type == SDL_KEYDOWN && !e.key.repeat) {
                switch (e.key.keysym.sym) {
                case SDLK_ESCAPE: quit = 1; break;
                case SDLK_F11:
                    SDL_SetWindowFullscreen(win, (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP) ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                    break;
                case SDLK_p:
                    if (!online) {
                        paused = !paused;
                        dev_diag_event(paused ? "GAME PAUSED" : "GAME RESUMED");
                    } else game_notice("Pause is disabled during online co-op.");
                    break;
                case SDLK_TAB: if (!online) emu_set_turbo(1); break;
                case SDLK_F10:
                    dev_diag_request_screenshot("manual F10");
                    game_notice("Diagnostic screenshot queued in the logs folder.");
                    break;
                case SDLK_F12: shot = 1; break;
                }
            } else if (!online && e.type == SDL_KEYUP && e.key.keysym.sym == SDLK_TAB) {
                emu_set_turbo(0);
            } else if (e.type == SDL_CONTROLLERBUTTONDOWN &&
                       e.cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE &&
                       !action && pad_is_selected_instance(g, e.cbutton.which)) {
                quit = 1;
            }
        }

        if (load_state_request_app[g]) {
            load_state_request_app[g] = 0;
            if (!online) {
                game_state_load(g, paused);
                f->seq = UINT64_MAX;
                have = 0;
            }
        }

        int ds_rewinding = !online && ds_menu.open && ds_menu.touch_active && ds_menu.selected == DS_MENU_REWIND;
        int now_rewind = !online && (app_rewind_held(g) || companion_rewind_held) && !paused;
        if (ds_rewinding) {
            emu_rewind_step();
            emu_mp_frame_refresh(f);
            have = 1;
        } else if (now_rewind) {
            if (!rewind_held) {
                if (emu_rewind_available()) { rewind_held = 1; render_reset(); }
                else game_notice("Rewind buffer is not ready yet.");
            }
            if (rewind_held) {
                emu_rewind_step();
                emu_mp_frame_refresh(f);
                have = 1;
            }
        } else {
            if (rewind_held) {
                emu_rewind_end();
                render_reset();
                rewind_held = 0;
            }
            if (!paused) {
                uint8_t synchronized_actions = 0;
                touch_apply_inputs(g, paused);
                if (online) {
                    /* Host owns Mario/P1; joining client owns Luigi/P2 on both replicas. */
                    pad_poll_player(g, 0, &buttons[0], &dpad[0]);
                    uint8_t p1b = 0, p1d = 0, p1a = 0, p2b = 0, p2d = 0, p2a = 0;
                    Uint64 exchange_started = SDL_GetPerformanceCounter();
                    int exchange_result = netplay_exchange_inputs(
                        online_frame, buttons[0], dpad[0], online_actions,
                        &p1b, &p1d, &p1a, &p2b, &p2d, &p2a);
                    Uint64 exchange_ticks = SDL_GetPerformanceCounter() - exchange_started;
                    Uint64 exchange_freq = SDL_GetPerformanceFrequency();
                    double exchange_ms = exchange_freq
                        ? (double)exchange_ticks * 1000.0 / (double)exchange_freq : 0.0;
                    if (exchange_ms >= 100.0) {
                        char perf_msg[160];
                        snprintf(perf_msg, sizeof perf_msg,
                                 "ONLINE_INPUT_WAIT frame=%llu ms=%.1f role=%s result=%d",
                                 (unsigned long long)online_frame, exchange_ms,
                                 netplay_is_host() ? "host" : "guest", exchange_result);
                        dev_diag_event(perf_msg);
                        fprintf(stderr, "[netplay] %s\n", perf_msg);
                    }
                    if (exchange_result != 0) {
                        quit = 1;
                        game_notice("Online session ended or input timed out.");
                    } else {
                        buttons[0] = p1b; dpad[0] = p1d;
                        buttons[1] = p2b; dpad[1] = p2d;
                        synchronized_actions = (uint8_t)(p1a | p2a);
                        online_actions = 0;
                        online_frame++;
                        if (p2a & NETPLAY_ACTION_RESPAWN_P2) emu_mp_request_respawn(1);
                    }
                } else {
                    for (int player = 0; player < player_count; player++)
                        pad_poll_player(g, player, &buttons[player], &dpad[player]);
                }
                if (!quit) {
                /* SML2's shared-world mode advances secondary characters from
                 * inside the original Player 1 frame, so inputs must be ready
                 * before the authoritative game update starts. */
                if (g == GAME_SML2)
                    for (int player = 1; player < player_count; player++)
                        emu_mp_set_player_input(player, buttons[player], dpad[player]);

                Uint64 p1_step_start = SDL_GetPerformanceCounter();
                int n0 = emu_mp_step(0, buttons[0], dpad[0], f, a0, 4096);
                Uint64 p1_step_ticks = SDL_GetPerformanceCounter() - p1_step_start;
                Uint64 step_freq = SDL_GetPerformanceFrequency();
                double p1_step_ms = step_freq
                    ? (double)p1_step_ticks * 1000.0 / (double)step_freq : 0.0;
                if (online && p1_step_ms >= 50.0) {
                    char perf_msg[160];
                    snprintf(perf_msg, sizeof perf_msg,
                             "ONLINE_SIM_SLOW player=1 frame=%llu ms=%.1f",
                             (unsigned long long)online_frame, p1_step_ms);
                    dev_diag_event(perf_msg);
                    fprintf(stderr, "[netplay] %s\n", perf_msg);
                }
                if (n0 < 0) {
                    quit = 1;
                } else {
                    int failed = 0;
                    for (int player = 1; player < player_count; player++) {
                        Frame *pf = player_frames[player - 1];
                        Uint64 p_step_start = SDL_GetPerformanceCounter();
                        int result = emu_mp_step(player, buttons[player], dpad[player], pf, NULL, 0);
                        Uint64 p_step_ticks = SDL_GetPerformanceCounter() - p_step_start;
                        double p_step_ms = step_freq
                            ? (double)p_step_ticks * 1000.0 / (double)step_freq : 0.0;
                        if (online && p_step_ms >= 50.0) {
                            char perf_msg[160];
                            snprintf(perf_msg, sizeof perf_msg,
                                     "ONLINE_SIM_SLOW player=%d frame=%llu ms=%.1f",
                                     player + 1, (unsigned long long)online_frame, p_step_ms);
                            dev_diag_event(perf_msg);
                            fprintf(stderr, "[netplay] %s\n", perf_msg);
                        }
                        if (result < 0) { failed = 1; break; }
                        /* SML2 shares one APU between all characters. Luigi's
                         * private movement pass restores shared sound-engine RAM,
                         * so play his captured one-shot events through his own bank. */
                        for (int event = 0; event < N_P2_SFX; event++)
                            if (pf->p2_sfx_events & (1u << event))
                                audio_mp_sfx(player + 1, event);
                    }
                    if (failed) {
                        quit = 1;
                    } else {
                        emu_mp_frame_refresh(f);
                        have = 1;
                        if (g == GAME_SML2) {
                            for (int player = 1; player < player_count; player++) {
                                if (f->mp_player_waiting_spawn[player]) {
                                    game_notice("Waiting for clean area to spawn...");
                                    break;
                                }
                            }
                        }
                        emu_rewind_capture();

                        /* Both replicas consume the same action bits on this
                         * frame. The host authors shared save bytes; the relay
                         * transfers that exact state to the joining player. */
                        if (online && (synchronized_actions & NETPLAY_ACTION_LOAD_STATE)) {
                            if (game_shared_state_load(g, netplay_shared_slot_active()) == 0) {
                                emu_mp_frame_refresh(f);
                                render_reset();
                                have = 1;
                            }
                        } else if (online && (synchronized_actions & NETPLAY_ACTION_SAVE_STATE)) {
                            (void)game_shared_state_save(g);
                        }

                        if (g == GAME_SML2 || f->game_state == 0) {
                            for (int player = 1; player < player_count; player++) {
                                if (!f->mp_player_visible[player]) continue;
                                if (g == GAME_SML) {
                                    render_overlay_sml1_effect_oam(f, f->mp_player_effect_oam[player], 0, 0);
                                    render_overlay_sml1_projectile_oam(f, f->mp_player_projectile_oam[player], 0, 0);
                                }
                                if (!f->mp_player_blink_hidden[player])
                                    render_overlay_sml1_mp_oam(f, f->mp_player_oam[player], 0, 0, player);
                            }
                        }
                        dev_diag_frame(f, g, player_count, buttons, dpad, paused);
                        if (n0 > 0) audio_game_push(a0, n0);
                        if (audio_ok()) audio_game_wait(audio_game_target());
                    }
                }
                }
            }
        }

        int ev;
        while ((ev = events_pop()) >= 0) pad_event_fx(g, ev);

        Uint64 now = SDL_GetPerformanceCounter();
        float dt = (float)(now - last) / SDL_GetPerformanceFrequency();
        last = now;
        int W, H;
        out_size(&W, &H);
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        draw_bg_cover((float)c->bg_dim);
        bg_update(dt);

        SDL_Rect game_rect = {0, 0, 0, 0};
        if (have) {
            render_build(f, g, 1);
            companion_update_pipe_swap(g, f, g == GAME_SML &&
                settings.launcher_dual_screen && settings.launcher_pipe_screen_swap);
            companion_publish_game_frame();
            render_fit(W, H, c->aspect, c->scaling, &game_rect);
            if (!companion_pipe_swap_active) render_draw(&game_rect, c->scaling);
            pad_set_screen_color(render_avg_color());
        }

        companion_publish_state(g, f, paused);
        capture_pending_state_thumbnail();
        ui_begin(W, H, dt);
        if (have && !companion_pipe_swap_active) {
            sml1_draw_player_tag(f, f->mp_player_oam[0], &game_rect, "Mario", C_ACCENT);
            for (int player = 1; player < player_count; player++) {
                if (!f->mp_player_visible[player] || f->mp_player_blink_hidden[player]) continue;
                sml1_draw_player_tag(f, f->mp_player_oam[player], &game_rect,
                                    mp_player_name(g, player), render_sml1_mp_player_color(player));
            }
            draw_coop_hud(g, f);
        }
        if (paused) {
            ui_rect(ui_view_x0(), ui_view_y0(), ui_view_w(), ui_view_h(), RGBA(0, 0, 0, 120));
            ui_text_c(F_BOLD, 32, UI_W / 2, UI_H / 2 - 20, C_TEXT, "Paused");
        }
        game_notice_draw(dt);
        touch_draw_overlay(paused);
        ds_menu_draw(g);
        ui_end();
        pad_frame(dt);

        if (shot) {
            shot = 0;
            SDL_Surface *snap = SDL_CreateRGBSurfaceWithFormat(0, W, H, 32, SDL_PIXELFORMAT_ARGB8888);
            if (snap) {
                SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_ARGB8888, snap->pixels, snap->pitch);
                char path[1200];
                snprintf(path, sizeof path, "%sscreenshot_%u.bmp", settings_dir(), SDL_GetTicks());
                SDL_SaveBMP(snap, path);
                SDL_FreeSurface(snap);
            }
        }
        /* Capture after the game and HUD are fully drawn, before SDL presents
         * the backbuffer. The Dev build only performs readback when an incident
         * or periodic screenshot has been requested. */
        dev_diag_capture_screen(ren);
        SDL_RenderPresent(ren);
    }

    companion_reset_pipe_swap();
    companion_set_display_enabled(0);
    touch_reset_game(g);
    if (online) netplay_game_end();
    dev_diag_end(quit == 2 ? "main window closed" : "multiplayer exited");
    emu_set_turbo(0);
    audio_game_end();
    emu_mp_end();
    tex_collect_save();
    free(f);
    for (int i = 0; i < MAX_MP_PLAYERS - 1; i++) free(player_frames[i]);
    pad_set_context(g, 0);
    ds_menu_close();
    SDL_SetWindowFullscreen(win, 0);
    SDL_SetWindowSize(win, win_w, win_h);
    SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    return quit == 2;
}

static int play_external_game(int g)
{
    if (g < 0 || g >= N_GAMES || !games[g].external_player) return 0;
    GameCfg *c = &settings.g[g];
    RomStatus st;
    if (!c->rom_path[0]) {
        launcher_toast("Choose this game's ROM on the Game tab first.");
        return 0;
    }
    if (rom_probe(g, c->rom_path, &st)) {
        launcher_toast(st.msg);
        return 0;
    }

    /* External Wario entries run in a separate process. Materialize the
     * selected hack as its own cached .gb file so the base ROM is never
     * written or passed to the external player in patched form. */
    const char *launch_rom = c->rom_path;
    char hack_copy[1200] = "";
    if (c->romhack_selected >= 0 &&
        c->romhack_selected < c->romhack_count &&
        c->romhack_selected < MAX_ROMHACKS) {
        char cache_dir[1200];
        snprintf(cache_dir, sizeof cache_dir, "%sromhacks_cache/", settings_dir());
        if (mkdir_u(cache_dir) != 0) {
            launcher_toast("Couldn't create the ROM hack cache folder.");
            return 0;
        }
        char profile_id[128];
        launcher_game_file_id(g, profile_id, sizeof profile_id);
        snprintf(hack_copy, sizeof hack_copy, "%s%s.gb", cache_dir, profile_id);
        if (rom_create_hack_copy(g, c->rom_path,
                                 c->romhacks[c->romhack_selected].path,
                                 hack_copy, &st) != 0) {
            launcher_toast(st.msg);
            return 0;
        }
        launch_rom = hack_copy;
    }

    char base[1200] = "";
    char player[1400];
    char *base_path = SDL_GetBasePath();
    if (base_path) {
        snprintf(base, sizeof base, "%s", base_path);
        SDL_free(base_path);
    }
#ifdef _WIN32
    snprintf(player, sizeof player, "%sgbplayer.exe", base);
#else
    snprintf(player, sizeof player, "%sgbplayer", base);
#endif

    FILE *test = fopen(player, "rb");
    if (!test) {
        launcher_toast("The bundled gbrecomp player is missing. Rebuild or reinstall PipeClean.");
        return 0;
    }
    fclose(test);
    game_session_begin(g);
    settings_save();
    audio_menu_music(0);
    int ok = 1;
#ifdef _WIN32
    char command[4096];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    memset(&pi, 0, sizeof pi);
    si.cb = sizeof si;
    /* ROM paths are ordinary Windows paths and are quoted to preserve spaces. */
    snprintf(command, sizeof command, "\"%s\" \"%s\"", player, launch_rom);
    if (CreateProcessA(player, command, NULL, NULL, FALSE, 0, NULL, base[0] ? base : NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        launcher_toast("Couldn't start the Wario game runtime.");
        ok = 0;
    }
#else
    pid_t pid = fork();
    if (pid == 0) {
        execl(player, player, launch_rom, (char *)NULL);
        _exit(127);
    }
    if (pid < 0) {
        launcher_toast("Couldn't start the Wario game runtime.");
        ok = 0;
    } else {
        int status = 0;
        while (waitpid(pid, &status, 0) < 0) {
            if (errno == EINTR) continue;
            launcher_toast("Couldn't wait for the Wario game runtime.");
            ok = 0;
            break;
        }
    }
#endif
    audio_menu_music(1);
    game_session_end();
    if (ok) settings_save();
    return 0;
}

/* returns 0 = back to launcher, 1 = quit */
static int play(int g)
{
    touch_reset_all();
    companion_reset_pipe_swap();
    companion_rewind_held = 0;
    ds_menu_close();
    if (g < 0 || g >= N_GAMES) return 0;
    int online = netplay_is_connected();
    if (online && g != GAME_SML && g != GAME_SML2) {
        launcher_toast("Online co-op currently supports Super Mario Land and Super Mario Land 2.");
        return 0;
    }
    if (games[g].external_player) return play_external_game(g);
    char err[256];
    GameCfg *c = &settings.g[g];
    load_state_request_app[g] = 0;

    /* SML/SML2 widen the ROM's entity activation/culling limits as part of
     * widescreen setup. Different per-PC widths can therefore make identical
     * online inputs evolve into different worlds. Install one canonical maximum
     * simulation width, then restore the user's preferred render width below. */
    int local_wide_l = 0, local_wide_r = 0;
    int sim_wide_l = 0, sim_wide_r = 0;
    int saved_wide = c->wide;
    if (online) {
        wide_dims(g, saved_wide, &local_wide_l, &local_wide_r);
        wide_dims(g, 100, &sim_wide_l, &sim_wide_r);
        c->wide = 100;
    }
    int prepare_failed = launcher_prepare(g, err, sizeof err);
    if (online) {
        c->wide = saved_wide;
        /* Keep each player's preferred visible width. Only ROM-side activation
         * hooks stay canonical, so host and guest simulate the same entities. */
        if (!prepare_failed && ppu_w == GB_W + sim_wide_l + sim_wide_r)
            ppu_set_wide(local_wide_l, local_wide_r,
                         games[g].hud_lines, games[g].hud_window, games[g].wide_gate);
    }
    if (prepare_failed) { launcher_toast(err); return 0; }
    if (online) {
        const CartInfo *ci = cart_info();
        if (!ci || netplay_game_begin(g, ci->crc) != 0) {
            NetplayInfo info;
            netplay_get_info(&info);
            launcher_toast(info.status[0] ? info.status : "Both players must start the same game ROM.");
            netplay_game_end();
            return 0;
        }
    }
    audio_menu_music(0);
    if ((g == GAME_SML || g == GAME_SML2) && (c->multiplayer || online)) {
        game_session_begin(g);
        int multiplayer_result = play_multiplayer(g);
        game_session_end();
        return multiplayer_result;
    }
    char sp[1200], save_id[128];
    launcher_game_file_id(g, save_id, sizeof save_id);
    snprintf(sp, sizeof sp, "%ssaves/", settings_dir());
    mkdir_u(sp);
    snprintf(sp, sizeof sp, "%ssaves/%s.sav", settings_dir(), save_id);
    emu_set_save_path(sp);
    bg_load(c->bg_path);
    texpack_load(c->tex_on ? c->tex_path : "");
    tex_collect_begin(g);
    render_reset();
    pad_set_context(g, 1);
    set_game_window(g);
    int requested_load = launcher_take_load_state(g);
    char suspended[1200];
    suspend_path(g, suspended, sizeof suspended);
    int has_suspend = state_file_exists(suspended);
    if (emu_start(0)) { launcher_toast("Couldn't start the game."); return 0; }
    game_session_begin(g);
    if (requested_load >= 0) {
        c->state_slot = requested_load;
        (void)game_state_load(g, 0);
    } else if (has_suspend) {
        if (emu_state_load_file(suspended, 1) == 0) remove(suspended);
    }

    Frame *f = SDL_malloc(sizeof *f);
    memset(f, 0, sizeof *f);
    int quit = 0, paused = 0, have = 0, rewind_held = 0;
    Uint64 last = SDL_GetPerformanceCounter();
    int shot = 0;
    while (!quit) {
        if (emu_cpu_faulted()) { quit = 1; break; }
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (companion_handle_event(g, &e, &paused, &quit)) continue;
            touch_handle_event(&e);
            pad_event(&e);
            int ds_consumed = ds_menu_event(g, &e, &paused, &quit);
            if (ds_consumed) continue;
            int action = app_handle_action_event(g, &e, &paused, &quit);
            if (e.type == SDL_QUIT) { quit = 2; }
            else if (e.type == SDL_KEYDOWN && !e.key.repeat) {
                switch (e.key.keysym.sym) {
                case SDLK_ESCAPE: quit = 1; break;
                case SDLK_F11: SDL_SetWindowFullscreen(win, (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP) ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP); break;
                case SDLK_p: paused = !paused; emu_set_paused(paused); break;
                case SDLK_TAB: emu_set_turbo(1); break;
                case SDLK_F12: shot = 1; break;
                }
            } else if (e.type == SDL_KEYUP && e.key.keysym.sym == SDLK_TAB) { emu_set_turbo(0); }
            else if (e.type == SDL_CONTROLLERBUTTONDOWN && e.cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE && !action && pad_is_selected_instance(g, e.cbutton.which)) quit = 1;
        }
        if (load_state_request_app[g]) {
            load_state_request_app[g] = 0;
            game_state_load(g, paused);
            f->seq = UINT64_MAX;
            have = 0;
        }
        if (ds_menu.open && ds_menu.touch_active && ds_menu.selected == DS_MENU_REWIND) {
            emu_rewind_step();
            if (emu_frame_get(f)) have = 1;
        }
        int now_rewind = (app_rewind_held(g) || companion_rewind_held) && !paused;
        if (now_rewind) {
            if (!rewind_held) {
                if (emu_rewind_available()) {
                    rewind_held = 1;
                    render_reset();
                } else {
                    game_notice("Rewind buffer is not ready yet.");
                }
            }
            if (rewind_held && emu_rewind_step() != 0) {
                game_notice("Rewind buffer is exhausted.");
                emu_rewind_end();
                render_reset();
                rewind_held = 0;
            }
        } else if (rewind_held) {
            emu_rewind_end();
            render_reset();
            rewind_held = 0;
        }
        touch_apply_inputs(g, paused);
        uint8_t b, d;
        pad_poll(g, &b, &d);
        emu_input(rewind_held ? 0 : b, rewind_held ? 0 : d);
        if (emu_frame_get(f)) { have = 1; tex_collect_frame(f); }
        int ev;
        while ((ev = events_pop()) >= 0) pad_event_fx(g, ev);
        Uint64 now = SDL_GetPerformanceCounter();
        float dt = (float)(now - last) / SDL_GetPerformanceFrequency();
        last = now;

        int W, H;
        out_size(&W, &H);
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        draw_bg_cover((float)c->bg_dim);
        bg_update(dt);
        if (have) {
            render_build(f, g, 1);
            companion_update_pipe_swap(g, f, g == GAME_SML &&
                settings.launcher_dual_screen && settings.launcher_pipe_screen_swap);
            companion_publish_game_frame();
            SDL_Rect r;
            render_fit(W, H, c->aspect, c->scaling, &r);
            if (!companion_pipe_swap_active) render_draw(&r, c->scaling);
            pad_set_screen_color(render_avg_color());
        }
        companion_publish_state(g, f, paused);
        capture_pending_state_thumbnail();
        ui_begin(W, H, dt);
        if (paused) {
            ui_rect(ui_view_x0(), ui_view_y0(), ui_view_w(), ui_view_h(), RGBA(0, 0, 0, 120));
            ui_text_c(F_BOLD, 32, UI_W / 2, UI_H / 2 - 20, C_TEXT, "Paused");
        }
        if (rewind_held) {
            ui_rrect(UI_W / 2 - 112, 18, 224, 34, 10, RGBA(20, 24, 35, 230));
            ui_text_c(F_BOLD, 13, UI_W / 2, 26, C_TEXT, "REWIND");
        }
        game_notice_draw(dt);
        touch_draw_overlay(paused);
        ds_menu_draw(g);
        ui_end();
        pad_frame(dt);
        if (shot) { shot = 0; SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, W, H, 32, SDL_PIXELFORMAT_ARGB8888);
                    if (s) { SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch);
                             char p[1200]; snprintf(p, sizeof p, "%sscreenshot_%u.bmp", settings_dir(), SDL_GetTicks()); SDL_SaveBMP(s, p); SDL_FreeSurface(s); } }
        SDL_RenderPresent(ren);
    }
    companion_reset_pipe_swap();
    companion_set_display_enabled(0);
    touch_reset_game(g);
    emu_stop();
    if (emu_cpu_faulted()) {
        uint8_t opcode = 0; uint16_t pc = 0; char msg[144];
        emu_cpu_fault_info(&opcode, &pc);
        snprintf(msg, sizeof msg, "Game stopped at unsupported opcode %02X (%04X); check the ROM or hack revision.", opcode, pc);
        launcher_toast(msg);
    }
    SDL_free(f);
    tex_collect_save();
    pad_set_context(g, 0);
    game_session_end();
    SDL_SetWindowFullscreen(win, 0);
    SDL_SetWindowSize(win, win_w, win_h);
    SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    return quit == 2;
}

int app_run(const char *autorun, const char *hack)
{
    (void)hack;
#ifdef _WIN32
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
#endif
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    /* Support both native finger events and SDL's touch-as-mouse fallback.
       The launcher deduplicates their button transitions, so Android devices
       that expose only one input path still get clickable UI controls. */
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK | SDL_INIT_EVENTS)) {
        fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return 1;
    }
    /* Keep native touch events enabled even when SDL also emits mouse events. */
    SDL_EventState(SDL_FINGERDOWN, SDL_ENABLE);
    SDL_EventState(SDL_FINGERMOTION, SDL_ENABLE);
    SDL_EventState(SDL_FINGERUP, SDL_ENABLE);
    int img_flags = IMG_INIT_PNG | IMG_INIT_JPG | IMG_INIT_TIF | IMG_INIT_WEBP;
    if ((IMG_Init(img_flags) & img_flags) != img_flags) {
        fprintf(stderr, "SDL_image: %s\n", IMG_GetError());
    }
    settings_load();
#if !defined(__ANDROID__)
    if (settings.launcher_window_size == 0) { win_w = 900; win_h = 580; }
    else if (settings.launcher_window_size == 2) { win_w = 1440; win_h = 900; }
#endif
    win = SDL_CreateWindow("PipeClean", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, win_w, win_h,
                           SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!win) { fprintf(stderr, "window: %s\n", SDL_GetError()); return 1; }
    SDL_SetWindowMinimumSize(win, 640, 420);
    if (getenv("GBL_SIZE")) { sscanf(getenv("GBL_SIZE"), "%dx%d", &win_w, &win_h); SDL_SetWindowSize(win, win_w, win_h); }
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!ren) { fprintf(stderr, "renderer: %s\n", SDL_GetError()); return 1; }
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    ui_init(ren);
    branding_init(ren, win);
    bg_init(ren);
    render_init(ren);
    launcher_set_renderer(ren);
    audio_init(settings.latency);
    pad_init();
    ui_sfx_cb = sfx;
    launcher_init();
    launcher_enter();
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);

    int quit = 0;
    if (autorun) {
        int id = rom_identify_file(autorun);
        if (id >= 0) {
            snprintf(settings.g[id].rom_path, sizeof settings.g[id].rom_path, "%s", autorun);
            quit = play(id);
            launcher_enter();
            launcher_display_cache_reset();
        }
    }
    Uint64 last = SDL_GetPerformanceCounter();
    while (!quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            /* Let the launcher relinquish its virtual controller cursor before
               the direct finger path presses the UI. Mouse events retain their
               existing dispatch order for normal desktop interaction. */
            if (e.type == SDL_FINGERDOWN || e.type == SDL_FINGERMOTION ||
                e.type == SDL_FINGERUP) {
                launcher_event(&e);
                launcher_touch_event(&e);
                pad_event(&e);
            } else {
                launcher_touch_event(&e);
                pad_event(&e);
                launcher_event(&e);
            }
            switch (e.type) {
            case SDL_QUIT: quit = 1; break;
#if defined(__ANDROID__)
            case SDL_USEREVENT:
                if (e.user.code == PIPECLEAN_LAYOUT_PICKER_EVENT_CODE) {
                    CompanionLayoutPickerResult *picked = (CompanionLayoutPickerResult *)e.user.data2;
                    launcher_companion_layout_picker_result((int)(intptr_t)e.user.data1,
                        picked ? picked->game : -1, picked ? picked->path : "");
                    free(picked);
                }
                break;
#endif
            case SDL_MOUSEBUTTONDOWN: if (e.button.button == SDL_BUTTON_LEFT) { mouse_sync(); ui_mouse_button(1); } break;
            case SDL_MOUSEBUTTONUP: if (e.button.button == SDL_BUTTON_LEFT) { mouse_sync(); ui_mouse_button(0); } break;
            case SDL_MOUSEWHEEL: ui_mouse.wheel += e.wheel.y; break;
            case SDL_DROPFILE: launcher_drop(e.drop.file); SDL_free(e.drop.file); break;
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_LEAVE) ui_mouse.inside = 0;
                if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                    if (launcher_touch_active) ui_mouse_button(0);
                    launcher_touch_active = 0;
                    launcher_touch_pointer = 0;
                }
                break;
            case SDL_APP_WILLENTERBACKGROUND:
            case SDL_APP_TERMINATING:
                if (launcher_touch_active) ui_mouse_button(0);
                launcher_touch_active = 0;
                launcher_touch_pointer = 0;
                ui_mouse.inside = 0;
                break;
            }
        }
        if (audio_music_take_finished()) launcher_menu_music_step(1);
        if (!launcher_touch_pointer) mouse_sync();
        Uint64 now = SDL_GetPerformanceCounter();
        float dt = (float)(now - last) / SDL_GetPerformanceFrequency();
        last = now;
        apply_launcher_display_preferences();
        int W, H;
        out_size(&W, &H);
        SDL_SetRenderDrawColor(ren, 14, 16, 22, 255);
        SDL_RenderClear(ren);
        ui_begin(W, H, dt);
        LauncherResult r = launcher_frame(dt);
        ui_end();
        if (getenv("GBL_SHOT")) {
            static int fr;
            if (fr == 3 && getenv("GBL_MOUSE")) { int mx, my; sscanf(getenv("GBL_MOUSE"), "%d,%d", &mx, &my); SDL_WarpMouseInWindow(win, mx, my); }
            if (++fr == 8) {
                SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, W, H, 32, SDL_PIXELFORMAT_ARGB8888);
                SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch);
                SDL_SaveBMP(s, getenv("GBL_SHOT"));
                printf("out %dx%d hint='%s' mouse=%.0f,%.0f\n", W, H, ui_hint_text(), ui_mouse.x, ui_mouse.y);
                quit = 1;
            }
        }
        SDL_RenderPresent(ren);
        companion_publish_launcher_dashboard(launcher_current_game());
        if (r.play >= 0) {
            audio_sfx(SFX_CONFIRM);
            settings_save();
            quit = play(r.play);
            launcher_enter();
            launcher_display_cache_reset();
            memset(&ui_mouse, 0, sizeof ui_mouse);
        }
        if (!(SDL_GetWindowFlags(win) & SDL_WINDOW_SHOWN)) SDL_Delay(30);
    }
    launcher_shutdown();
    branding_shutdown();
    settings_save();
    pad_shutdown();
    audio_shutdown();
    render_shutdown();
    bg_shutdown();
    ui_shutdown();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    IMG_Quit();
    SDL_Quit();
    return 0;
}