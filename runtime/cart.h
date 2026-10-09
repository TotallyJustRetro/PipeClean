#pragma once
#include <stdint.h>
#include <stddef.h>
typedef struct { char title[17]; int mapper; int rom_banks; size_t ram_bytes; int battery; size_t size; uint32_t crc; uint8_t cgb_flag; } CartInfo;
int cart_parse(const uint8_t *img,size_t n,CartInfo *ci); int cart_install(const uint8_t *img,size_t n); void cart_reset(void); const CartInfo *cart_info(void); int cart_load_save(const char *path); int cart_write_save(const char *path);
size_t cart_state_size(void);
int cart_state_save(void *dst, size_t n);
int cart_state_load(const void *src, size_t n);
/* External cartridge RAM is emulated work/save memory and must travel with save states. */
size_t cart_ram_state_size(void);
int cart_ram_state_save(void *dst, size_t n);
int cart_ram_state_load(const void *src, size_t n);
extern const uint8_t *cart_lo,*cart_hi; void cart_write_ctrl(uint16_t a,uint8_t v); uint8_t cart_ram_read(uint16_t a); void cart_ram_write(uint16_t a,uint8_t v);