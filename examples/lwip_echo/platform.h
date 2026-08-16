/*
 * Copyright (C) 2009 - 2019 Xilinx, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
 * SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 *
 */

/*
 * Local override of the Vitis lwIP Echo Server template's platform.h.
 * Only ETH_LINK_DETECT_INTERVAL differs from the stock file; everything else
 * is verbatim, including the copyright above.
 *
 * ETH_LINK_DETECT_INTERVAL is set beyond reach to DISABLE the periodic link
 * monitor, which on this board destroys the link it is meant to watch:
 * eth_link_detect() -> xadapter.c's state machine calls
 * phy_setup_emacps() on every transition to link-up, and with a pinned
 * phy_link_speed that lands in configure_IEEE_phy_speed(), which rewrites the
 * control register and re-advertises. That restarts negotiation, the link
 * drops, and one second later the monitor "recovers" it by doing the same
 * thing again. Observed as an endless
 *
 *     link speed for phy address 1: 100 / Ethernet Link up / Ethernet Link down
 *
 * at both 100 and 1000 Mbps, so it is not an RGMII timing problem — it is the
 * monitor fighting itself. The link established at init is fine; nothing else
 * disturbs it.
 *
 * The stock value is 4, meaning once per second (the platform timer ticks at
 * 250 ms). A value the counter never reaches disables the monitor without
 * touching the vendor's .c files.
 *
 * COST: no automatic recovery from a cable pull — the link is brought up once,
 * at init, and stays as configured. Acceptable here: this is a fixed-install
 * updater on a pinned link speed, not a general-purpose NIC. If recovery is
 * ever needed, the real fix is upstream, in the Realtek path of
 * xemacpsif_physpeed.c (see the note in project.mk).
 */

#ifndef __PLATFORM_H_
#define __PLATFORM_H_

#define ETH_LINK_DETECT_INTERVAL 0x40000000

void init_platform();
void cleanup_platform();
#ifdef __MICROBLAZE__
void timer_callback();
#endif
#ifdef __PPC__
void timer_callback();
#endif
void platform_setup_timer();
void platform_enable_interrupts();
#endif
