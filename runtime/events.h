#pragma once
#include <stdint.h>

typedef struct {
    const char *id;
    const char *name;
    const char *desc;
    int sfx_mask;
    int sfx_ms;
    int beep;
    uint32_t color;
    int rumble_ms, rumble_str;
} EventDef;

int  events_count(int game);
const EventDef *events_def(int game, int i);
void events_begin(void);
void events_end(void);
void events_frame(void);
void events_state_reset(void);
int  events_pop(void);
extern int events_log;