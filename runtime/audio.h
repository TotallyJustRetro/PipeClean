#pragma once
#include <stdint.h>
typedef struct { unsigned underruns; int fill_frames; int target_frames; int device_rate, device_samples; int playing; } AudioStats;
int audio_init(int latency_mode); void audio_shutdown(void); int audio_rate(void); int audio_ok(void);
void audio_game_begin(void); void audio_game_end(void); void audio_game_flush(void); void audio_game_push(const int16_t *stereo,int frames); int audio_game_target(void); void audio_game_wait(int target_frames); void audio_game_abort(void); void audio_game_set_paused(int p);
enum { SFX_HOVER,SFX_CLICK,SFX_CONFIRM,SFX_BACK,SFX_TOGGLE };
void audio_menu_apply(void); void audio_menu_music(int on); void audio_sfx(int which); void audio_sfx_preview(int which); void audio_music_preview(void); void audio_get_stats(AudioStats *s);
void audio_p2_sfx(int event); void audio_p2_sfx_preview(int event);
int audio_pad_open(void); void audio_pad_close(void); int audio_pad_available(void); void audio_pad_set_gain(float speaker,float haptics); void audio_pad_push(const int16_t *stereo,int frames); void audio_pad_play_beep(int kind);