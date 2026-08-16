#pragma once
#include "xil_types.h"
#define SHA256_DIGEST_LEN 32
#define SHA256_HEX_LEN 65
typedef struct { int x; } sha256_ctx_t;
static inline void sha256_init(sha256_ctx_t*c){(void)c;}
static inline void sha256_update(sha256_ctx_t*c,const u8*d,u32 n){(void)c;(void)d;(void)n;}
static inline void sha256_final(sha256_ctx_t*c,u8*o){(void)c;for(int i=0;i<32;i++)o[i]=0;}
static inline void sha256_hex(const u8*d,char*h){for(int i=0;i<32;i++){static const char*x="0123456789abcdef";h[i*2]=x[d[i]>>4];h[i*2+1]=x[d[i]&15];}h[64]=0;}
