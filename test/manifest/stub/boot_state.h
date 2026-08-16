#pragma once
#include <stdint.h>
#define BOOT_ATTEMPTS_MAX 3
typedef struct { uint32_t magic, update_present, boot_attempts, installed_version, crc32; } boot_state_t;
typedef enum { BS_OK=0, BS_DEFAULTED, BS_LEGACY, BS_ERR_FLASH, BS_ERR_ARG } bs_result_t;
static inline const char *bs_result_str(bs_result_t r){(void)r;return "";}
static inline bs_result_t boot_state_read(boot_state_t*o){o->magic=0;o->update_present=0;o->boot_attempts=0;o->installed_version=0;o->crc32=0;return BS_OK;}
static inline bs_result_t boot_state_write(const boot_state_t*i){(void)i;return BS_OK;}
