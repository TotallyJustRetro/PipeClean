#include <errno.h>
#include "util.h"
#ifdef __ANDROID__
#include <SDL.h>
#include <SDL_system.h>
#include <jni.h>
#include <pthread.h>
#endif
#undef fopen
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <shlobj.h>
#include <commdlg.h>

static int to_w(const char *s, wchar_t *w, int n) { return MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n); }
static void from_w(const wchar_t *w, char *s, size_t n) { WideCharToMultiByte(CP_UTF8, 0, w, -1, s, (int)n, NULL, NULL); }

FILE *fopen_u(const char *path, const char *mode)
{
    wchar_t wp[2048], wm[16];
    if (!to_w(path, wp, 2048) || !to_w(mode, wm, 16)) return fopen(path, mode);
    return _wfopen(wp, wm);
}
int mkdir_u(const char *path)
{
    wchar_t wp[2048];
    if (!to_w(path, wp, 2048)) return -1;
    return (_wmkdir(wp) == 0 || errno == EEXIST) ? 0 : -1;
}
int file_exists(const char *path)
{
    wchar_t wp[2048];
    if (!to_w(path, wp, 2048)) return 0;
    DWORD a = GetFileAttributesW(wp);
    return a != INVALID_FILE_ATTRIBUTES;
}
#else
FILE *fopen_u(const char *path, const char *mode) { return fopen(path, mode); }
int mkdir_u(const char *path) { return mkdir(path, 0755) == 0 || file_exists(path) ? 0 : -1; }
int file_exists(const char *path) { struct stat st; return stat(path, &st) == 0; }
#endif

const char *path_base(const char *p)
{
    const char *a = strrchr(p, '/'), *b = strrchr(p, '\\');
    if (b > a) a = b;
    return a ? a + 1 : p;
}

#ifdef _WIN32
static int dlg_win(int kind, const char *title, char *out, size_t n)
{
    wchar_t wtitle[256], buf[2048] = L"";
    to_w(title, wtitle, 256);
    static int com;
    if (!com) { CoInitializeEx(NULL, COINIT_APARTMENTTHREADED); com = 1; }
    if (kind == DLG_FOLDER) {
        BROWSEINFOW bi;
        memset(&bi, 0, sizeof bi);
        bi.lpszTitle = wtitle;
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        LPITEMIDLIST id = SHBrowseForFolderW(&bi);
        if (!id) return 0;
        int ok = SHGetPathFromIDListW(id, buf);
        CoTaskMemFree(id);
        if (!ok) return 0;
        from_w(buf, out, n);
        return 1;
    }
    static const wchar_t *filters[] = {
        L"Game Boy / Game Boy Advance ROM (*.gb;*.gbc;*.gba)\0*.gb;*.gbc;*.gba\0All files\0*.*\0",
        L"Romhack or patch (*.ips;*.bps;*.ups;*.gb;*.gbc)\0*.ips;*.bps;*.ups;*.gb;*.gbc\0All files\0*.*\0",
        L"Images (*.png;*.gif)\0*.png;*.gif\0All files\0*.*\0",
        L"Audio (*.wav;*.mp3;*.ogg;*.flac)\0*.wav;*.mp3;*.ogg;*.flac\0All files\0*.*\0",
        L"All files\0*.*\0",
        L"ZIP archive (*.zip)\0*.zip\0All files\0*.*\0",
        L"ZIP archive (*.zip)\0*.zip\0All files\0*.*\0"};
    OPENFILENAMEW o;
    memset(&o, 0, sizeof o);
    o.lStructSize = sizeof o;
    o.lpstrFilter = filters[kind];
    o.lpstrFile = buf;
    o.nMaxFile = 2048;
    o.lpstrTitle = wtitle;
    o.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (kind == DLG_ZIP_SAVE) {
        o.lpstrDefExt = L"zip";
        o.Flags |= OFN_OVERWRITEPROMPT;
        if (!GetSaveFileNameW(&o)) return 0;
    } else {
        o.Flags |= OFN_FILEMUSTEXIST;
        if (!GetOpenFileNameW(&o)) return 0;
    }
    from_w(buf, out, n);
    return 1;
}
int dlg_pick(int kind, const char *title, char *out, size_t n) { out[0] = 0; return dlg_win(kind, title, out, n); }
void open_folder(const char *path)
{
    wchar_t wp[2048];
    if (to_w(path, wp, 2048)) ShellExecuteW(NULL, L"open", wp, NULL, NULL, SW_SHOWNORMAL);
}
#elif defined(__ANDROID__)
static pthread_mutex_t android_picker_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t android_picker_cond = PTHREAD_COND_INITIALIZER;
static int android_picker_done;
static char android_picker_result[4096];

JNIEXPORT void JNICALL
Java_com_pipeclean_app_PipeCleanActivity_nativeDocumentPicked(JNIEnv *env, jclass cls, jstring path)
{
    (void)cls;
    const char *utf = path ? (*env)->GetStringUTFChars(env, path, NULL) : NULL;
    pthread_mutex_lock(&android_picker_mutex);
    if (utf) {
        snprintf(android_picker_result, sizeof android_picker_result, "%s", utf);
        (*env)->ReleaseStringUTFChars(env, path, utf);
    } else {
        android_picker_result[0] = 0;
    }
    android_picker_done = 1;
    pthread_cond_signal(&android_picker_cond);
    pthread_mutex_unlock(&android_picker_mutex);
}

int dlg_pick(int kind, const char *title, char *out, size_t n)
{
    if (!out || !n) return 0;
    out[0] = 0;
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    jobject activity = (jobject)SDL_AndroidGetActivity();
    if (!env || !activity) return 0;

    pthread_mutex_lock(&android_picker_mutex);
    android_picker_done = 0;
    android_picker_result[0] = 0;
    pthread_mutex_unlock(&android_picker_mutex);

    jclass cls = (*env)->GetObjectClass(env, activity);
    jmethodID method = cls ? (*env)->GetMethodID(env, cls, "openPipeCleanDocumentPicker",
                                                   "(ILjava/lang/String;)V") : NULL;
    jstring jtitle = (*env)->NewStringUTF(env, title ? title : "Choose a file");
    if (!method || !jtitle) {
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        if (jtitle) (*env)->DeleteLocalRef(env, jtitle);
        if (cls) (*env)->DeleteLocalRef(env, cls);
        return 0;
    }
    (*env)->CallVoidMethod(env, activity, method, (jint)kind, jtitle);
    (*env)->DeleteLocalRef(env, jtitle);
    (*env)->DeleteLocalRef(env, cls);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        pthread_mutex_lock(&android_picker_mutex);
        android_picker_done = 1;
        android_picker_result[0] = 0;
        pthread_mutex_unlock(&android_picker_mutex);
    }

    pthread_mutex_lock(&android_picker_mutex);
    while (!android_picker_done) pthread_cond_wait(&android_picker_cond, &android_picker_mutex);
    snprintf(out, n, "%s", android_picker_result);
    pthread_mutex_unlock(&android_picker_mutex);
    return out[0] != 0;
}

void open_folder(const char *path) { (void)path; }
#else
int dlg_pick(int kind, const char *title, char *out, size_t n)
{
    out[0] = 0;
    char cmd[1400];
#ifdef __APPLE__
    if (kind == DLG_ZIP_SAVE)
        snprintf(cmd, sizeof cmd, "osascript -e 'POSIX path of (choose file name with prompt \"%s\" default name \"PipeClean-CompanionDeck.zip\")' 2>/dev/null", title);
    else
        snprintf(cmd, sizeof cmd, "osascript -e 'POSIX path of (choose %s with prompt \"%s\")' 2>/dev/null",
                 kind == DLG_FOLDER ? "folder" : "file", title);
#else
    const char *filter = (kind == DLG_IMAGE) ? "PNG or GIF | *.png *.gif" :
        (kind == DLG_AUDIO) ? "Audio | *.wav *.mp3 *.ogg *.flac" :
        (kind == DLG_ZIP_OPEN || kind == DLG_ZIP_SAVE) ? "ZIP archive | *.zip" : "All files | *";
    const char *save_arg = kind == DLG_ZIP_SAVE ? "--save --confirm-overwrite" : "";
    const char *kdialog_mode = kind == DLG_FOLDER ? "--getexistingdirectory" :
        kind == DLG_ZIP_SAVE ? "--getsavefilename" : "--getopenfilename";
    snprintf(cmd, sizeof cmd,
        "zenity --file-selection %s --title=\"%s\" --file-filter='%s' 2>/dev/null || "
        "kdialog %s . '%s' 2>/dev/null",
        kind == DLG_FOLDER ? "--directory" : save_arg, title, filter, kdialog_mode, filter);
#endif
    FILE *p = popen(cmd, "r");
    if (!p) return 0;
    if (!fgets(out, (int)n, p)) { pclose(p); out[0] = 0; return 0; }
    pclose(p);
    size_t l = strlen(out);
    while (l && (out[l - 1] == '\n' || out[l - 1] == '\r')) out[--l] = 0;
    return l > 0;
}
void open_folder(const char *path)
{
    char cmd[1200];
#ifdef __APPLE__
    snprintf(cmd, sizeof cmd, "open \"%s\" >/dev/null 2>&1 &", path);
#else
    snprintf(cmd, sizeof cmd, "xdg-open \"%s\" >/dev/null 2>&1 &", path);
#endif
    if (system(cmd)) {}
}
#endif