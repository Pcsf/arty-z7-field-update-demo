#pragma once
#include "xil_types.h"
#define QSPI_WRITE_FLOOR 0x700000U
#define QSPI_WRITE_CEIL  0xFF0000U
#define QSPI_SECTOR_SIZE 0x10000U
#define QSPI_PAGE_SIZE   256U
typedef enum { QSPI_OK=0, QSPI_ERR } qspi_result_t;
static inline const char *qspi_result_str(qspi_result_t r){(void)r;return "";}
static inline qspi_result_t qspi_flash_read(u32 a,u8*b,u32 n){(void)a;(void)b;(void)n;return QSPI_OK;}
static inline qspi_result_t qspi_flash_erase_sector(u32 a){(void)a;return QSPI_OK;}
static inline qspi_result_t qspi_flash_program(u32 a,const u8*b,u32 n){(void)a;(void)b;(void)n;return QSPI_OK;}
