#pragma once
#include "xil_types.h"
#include "lwip/ip_addr.h"
#include "netif/xadapter.h"
typedef enum { TFTP_OK=0, TFTP_ERR } tftp_result_t;
static inline const char *tftp_result_str(tftp_result_t r){(void)r;return "";}
static inline tftp_result_t tftp_get(struct netif*n,const ip_addr_t*s,const char*f,u8*b,u32 l,u32*g){(void)n;(void)s;(void)f;(void)b;(void)l;*g=0;return TFTP_OK;}
