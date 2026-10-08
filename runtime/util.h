#pragma once
#include <stdio.h>
#include <stddef.h>
FILE *fopen_u(const char *path, const char *mode);
int mkdir_u(const char *path);
int file_exists(const char *path);
const char *path_base(const char *path);
#ifdef _WIN32
#define fopen fopen_u
#endif
enum { DLG_ROM, DLG_PATCH, DLG_IMAGE, DLG_AUDIO, DLG_FOLDER };
int dlg_pick(int kind, const char *title, char *out, size_t n);
void open_folder(const char *path);