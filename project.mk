# ==============================================================================
# project.mk — TFTP Field-Update Project Configuration
#
# This project uses the makefile_project_template as a git submodule (mk/).
# Edit this file to configure the build — never edit generated Makefile.mk files.
#
# Toolchains:
#   ghdl / modelsim — VHDL simulation
#   vivado         — synthesis, implementation, Vitis software
#   gcc            — bare-metal C
#   bootgen        — boot images
#
# Switch TOOLCHAIN to match the work in hand:
#   ghdl    → VHDL simulation
#   modelsim → VHDL simulation with ModelSim/Questa
#   vivado  → FPGA synthesis/implementation
#   gcc     → C compilation for bare-metal SW
# ==============================================================================

# ── Output ────────────────────────────────────────────────────────────────────
PROJECT_NAME := tftp_field_update
BUILD_DIR    := build
SRC_ROOT     := .

# ── Toolchain ─────────────────────────────────────────────────────────────────
# Set this to match the work in hand, then just run 'make' — that is
# the template's workflow (step 1 of 'make help'), and every target below
# follows from it.
#
#   ghdl     → VHDL simulation     'make' = simulate tb_blinkctl
#   modelsim → ModelSim / Questa simulation  'make' = simulate tb_blinkctl
#   vivado   → Vivado             'make' = synth + impl + bitstream
#                                            'make sim' = XSim behavioural
#   gcc      → bare-metal C       'make' = compile sw/
#
# Two things to know when this is set to 'vivado':
#   1. The Xilinx tools are not installed on the host — they live in the
#      vivado-image container (xilinx-dev-env), which mounts this repo at
#      /home/fpgauser/workspace. Every vivado target must be run from inside
#      it. The preflight in the root Makefile says so when you forget, rather
#      than letting it fail as a bare "xvhdl: command not found".
#   2. Plain 'make' becomes synth + impl + bitstream, which cannot complete
#      until VIVADO_TOP and VIVADO_XDC are set. Under vivado
#      today, use 'make sim', 'make sim-gui', 'make red'.
TOOLCHAIN := vivado

# ── VHDL compilation order ────────────────────────────────────────────────────
# Layer 1 (cross-directory): package dir → rtl dir → tb dir
VHDL_SRCS_DIR := \
    rtl      \
    tb

# vivado/arty holds bench_top.vhd, which instantiates the generated jtag_axi_0
# IP. It is a real synthesis source but it cannot be analysed by GHDL or
# ModelSim — the IP does not exist outside a Vivado project — so it joins the
# source list only for the Vivado toolchain. Kept last: it is the top level.
ifeq ($(TOOLCHAIN),vivado)
VHDL_SRCS_DIR += vivado/arty
endif

# ── GHDL settings ─────────────────────────────────────────────────────────────
GHDL       := ghdl
GHDL_STD   := 08
GHDL_FLAGS :=
# Top-level testbench entity
GHDL_TOP   := tb_blinkctl
# mk/make/ghdl.mk already emits build/$(PROJECT_NAME).vcd
GHDL_SIM_FLAGS :=

# ── ModelSim / QuestaSim settings ─────────────────────────────────────────────
VLIB      := vlib
VMAP      := vmap
VCOM      := vcom
VLOG      := vlog
VSIM      := vsim
VSIM_WORK := work
VSIM_TOP  := tb_blinkctl
VSIM_FLAGS := -t 1ns -voptargs=+acc

# ── Xilinx Vivado settings ────────────────────────────────────────────────────
VIVADO := vivado

# XSim behavioural simulation top (make sim / sim-gui / red).
VIVADO_SIM_TOP := tb_blinkctl

# The completion marker both testbenches print as their last act — tb_blinkctl
# reports "TEST COMPLETE", tb_blinkctl_axil "WRAPPER TEST COMPLETE", and this
# matches either. The framework fails 'make sim' when it is absent, which is the
# case a failing assert cannot cover: a run that stops early leaves nothing to
# match and would otherwise pass on xsim's meaningless exit 0.
XSIM_PASS_PATTERN := TEST COMPLETE

# Board files are vendored so a fresh clone builds without touching the Vivado
# installation — Digilent boards are not shipped with Vivado.
VIVADO_BOARD_REPO := vivado/board_files
VIVADO_BOARD_PART := digilentinc.com:arty-z7-20:part0:1.1

# Simulation-only sources — everything under tb/, by convention and without
# exception. Nothing in tb/ is ever synthesized; that includes blinkctl_stub.vhd,
# a second architecture of the blinkctl entity that exists purely for the TDD RED
# phase and which synthesis would otherwise bind in place of the real design.
#
# Deferred '=' is required, not cosmetic: VHDL_SRCS is populated by the framework
# AFTER project.mk is read, so ':=' would evaluate to empty here and silently push
# every testbench into the synthesis fileset.
VIVADO_SIM_SRCS = $(filter tb/%,$(VHDL_SRCS))

# VHDL-2008 everywhere (the template default); no exceptions needed. The
# VHDL-93 exemption only applies to the top file of an IP Integrator module
# reference, and nothing here is one — DESIGN=ps has a block design, but the
# RTL instantiates its generated wrapper rather than being referenced by it.
VIVADO_VHDL93 :=

# ── Payload version ───────────────────────────────────────────────────────────
# v1 = 1 Hz, v2 = 4 Hz (ICD §Versioning). Override: make PAYLOAD=v2 bitstream
PAYLOAD ?= v1

# ONE declaration of what each payload's VERSION register reads, used by both
# the bitstream generic below and the update manifest host-side. Fault-campaign
# update.sha carries a version field; a release script with its own
# hand-typed copy of this number would be a second source of truth for the one
# value the board compares against.
#
PAYLOAD_VERSION_v1 := 0x00010001
PAYLOAD_VERSION_v2 := 0x00020001

ifeq ($(PAYLOAD),v1)
  VIVADO_GENERICS := G_VERSION=32'h00010001 G_BLINK_DIV_RST=125000000
else ifeq ($(PAYLOAD),v2)
  VIVADO_GENERICS := G_VERSION=32'h00020001 G_BLINK_DIV_RST=31250000
else
  $(error PAYLOAD must be v1 or v2, got '$(PAYLOAD)')
endif

# Which payload the published update carries. update.bif names blinkctl_v2.bit,
# so this is v2 and moves with it; host/release.sh reads UPDATE_VERSION rather
# than knowing any of this.
UPDATE_PAYLOAD ?= v2
UPDATE_VERSION := $(PAYLOAD_VERSION_$(UPDATE_PAYLOAD))

# ── Design selection ──────────────────────────────────────────────────────────
# ps    — the design this demo ships. PS7 drives blinkctl over M_AXI_GP0; it is
#         what gets exported as an XSA, built into BOOT.BIN, and booted from
#         QSPI. Every documented command uses it, so it is the default.
# bench — PS-less hardware bench: JTAG-to-AXI drives blinkctl straight off the
#         board's 125 MHz PL oscillator. No PS, no DDR, no block design. Fast to
#         build and useful for exercising the peripheral on silicon by itself:
#         'make DESIGN=bench bitstream'.
DESIGN ?= ps
ifeq ($(DESIGN),bench)
  VIVADO_TOP := bench_top
  VIVADO_XDC := vivado/arty/arty_z7_bench.xdc
  VIVADO_IP  := jtag_axi_0
  # AXI4-Lite (PROTOCOL 2) matches blinkctl exactly — no protocol conversion
  # and no interconnect anywhere in this design.
  VIVADO_IP_jtag_axi_0_VLNV   := xilinx.com:ip:jtag_axi:1.2
  VIVADO_IP_jtag_axi_0_CONFIG := CONFIG.PROTOCOL=2 CONFIG.M_AXI_ADDR_WIDTH=32
else ifeq ($(DESIGN),ps)
  # ps — PS7-based design: the CPU drives blinkctl over M_AXI_GP0. This is what
  #      is exported as an XSA (Vitis platform) and loaded from
  #      BOOT.BIN. Still plain RTL plus create_ip — no block design.
  VIVADO_TOP := ps_top
  # LED pins only. No clock constraint: the PL clock comes from FCLK_CLK0 inside
  # the PS7, not from a package pin, and the IP declares its own period.
  VIVADO_XDC := vivado/arty/arty_z7.xdc
  # The PS7 lives in a block design, alone. Not for wiring convenience — the
  # fabric is still RTL — but because write_hw_platform derives its .hwh
  # hardware handoff from a block design. A pure-RTL export produced an XSA
  # containing only the bitstream, which Vitis cannot turn into a bare-metal
  # platform or an FSBL, and BOOT.BIN needs both.
  #
  # blinkctl stays out of the block design, so it will not appear in
  # xparameters.h. That is deliberate: doc/icd_blinkctl.md is the source of
  # truth for the register map, and software uses 0x43C0_0000 from there, the
  # same address the bench design has always used.
  VIVADO_BD          := ps_bd
  # Versioned block-design export and the source of truth once it exists. Until
  # then the VIVADO_BD_* keys below bootstrap it. The round trip is
  # 'make bd-draft' → edit and save in the IDE → 'make bd-export' → commit.
  VIVADO_BD_TCL      := vivado/bd/ps_bd.tcl
  VIVADO_BD_CELLS    := ps7_0
  VIVADO_BD_EXT_INTF := ps7_0/DDR ps7_0/FIXED_IO ps7_0/M_AXI_GP0
  VIVADO_BD_EXT_PINS := ps7_0/FCLK_CLK0 ps7_0/FCLK_RESET0_N
  # GP0's clock input is fed by the PS's own fabric clock.
  VIVADO_BD_NETS     := ps7_0/M_AXI_GP0_ACLK=ps7_0/FCLK_CLK0
  # The external port does not inherit FCLK's overridden rate, and
  # validate_bd_design fails on the mismatch (100 MHz port vs 125 MHz interface).
  VIVADO_BD_INTF_FREQ := M_AXI_GP0_0=125000000
  VIVADO_IP  := axi_pc_0
  VIVADO_IP_ps7_0_VLNV := xilinx.com:ip:processing_system7:5.5
  # Take Digilent's whole board preset — MIO mapping, DDR timings, peripheral
  # enables — rather than hand-deriving it. Vivado's own
  # CONFIG.PCW_IMPORT_BOARD_PRESET does nothing on a create_ip IP (verified on
  # 2021.2: UART0/ENET0/QSPI all still 0 afterwards), so the framework expands
  # the preset's parameters itself.
  VIVADO_IP_ps7_0_PRESET := vivado/board_files/arty-z7-20/A.0/preset.xml
  # Applied AFTER the preset, so these win. The preset asks for FCLK_CLK0 =
  # 100 MHz, but doc/icd_blinkctl.md specifies 125 MHz on the Arty and the
  # PAYLOAD divider constants are computed for it. Overriding the clock keeps
  # one ICD and one set of constants across both designs.
  VIVADO_IP_ps7_0_CONFIG := CONFIG.PCW_FPGA0_PERIPHERAL_FREQMHZ=125 CONFIG.PCW_USE_M_AXI_GP0=1
  # M_AXI_GP0 is AXI3 with 12-bit IDs; blinkctl_axil is AXI4-Lite. bench_top
  # needed no converter because JTAG-to-AXI can be AXI4-Lite outright.
  # ID_WIDTH is NOT optional: left at the default 0 the IP exposes no s_axi_*id
  # ports at all, and PS7's BID/RID inputs would have to be tied off by hand —
  # but AXI requires each response to carry its request's ID. At 12 the
  # converter absorbs and echoes them itself.
  # ── Vitis software on the exported platform ─────────────────────────────────
  # xsct is not on PATH under Vivado's settings64.sh.
  XSCT                  := /tools/Xilinx/Vitis/2021.2/bin/xsct
  VITIS_PLATFORM        := ps_plat
  VITIS_APPS            := regs lwip_echo tftp_client qspi sha256 multiboot \
                           multiboot_b updater
  VITIS_APP_regs_SRC    := examples/regs

  # The Arty's RTL8211E PHY hangs off PS GEM0 via MIO, so
  # the PL is not involved in Ethernet on this board and the app is pure PS.
  #
  # Built from Xilinx's own template rather than hand-written: the point of this
  # example is to prove the PHY, the MAC and the stack, not to reimplement an
  # echo server. Verified present in 2021.2 with 'repo -apps' — it supports
  # ps7_cortexa9/standalone. No _SRC is set, so the template's sources are used
  # as-is; add one later to overlay a modified main.c.
  VITIS_APP_lwip_echo_TEMPLATE := lwIP Echo Server
  # Overlays platform.h onto the template's sources — see that file for why the
  # periodic link monitor has to be disabled on this board.
  VITIS_APP_lwip_echo_SRC      := examples/lwip_echo

  # The BSP settles the question the guide left open: this
  # lwIP does NOT ship a TFTP client. lwip211_v1_6 carries
  # src/apps/tftp/tftp_server.c and its two headers, tftp_init() is the server
  # entry point, and a grep for tftp_client/tftp_init_client across the whole
  # library returns zero hits. So the read side is hand-written on the raw UDP
  # API, as expected.
  #
  # Built from the echo template rather than an Empty Application, and for the
  # template's network bring-up alone: xemac_add(), the SCU-timer platform layer
  # and the RTL8211E link path cost a full session to get right on this board.
  # The overlay replaces main.c and adds tftp_client.[ch]; the template's echo.c
  # stays in the project and is never called, so no TCP listener is opened.
  # (An Empty Application would work — VITIS_LIBS already puts lwip211 in the
  # BSP — it would just mean re-deriving the bring-up for nothing.)
  VITIS_APP_tftp_client_TEMPLATE := lwIP Echo Server
  VITIS_APP_tftp_client_SRC      := examples/tftp_client \
                                    sw/lib/tftp \
                                    sw/lib/netpump

  # No TEMPLATE key, so this takes the framework default
  # Empty Application(C) — the same shape as 'regs', and for the same reason:
  # nothing here needs the network. Overlaying onto the echo template would mean
  # inheriting a bring-up whose only job would be to be disabled.
  #
  # No platform regeneration is needed for this app, which is worth stating
  # because the instinct says otherwise. QSPI is already enabled in the block
  # design (PCW_EN_QSPI=1, single slave select on MIO 1..6 with the feedback
  # clock on MIO 8), so the BSP already carries qspips_v3_9 and xparameters.h
  # already defines XPAR_XQSPIPS_0_DEVICE_ID / _BASEADDR (0xE000D000) and
  # XPAR_XQSPIPS_0_QSPI_CLK_FREQ_HZ (200 MHz). Verified by grep before writing a
  # line of it; 'rm -rf build/vitis_ws' would cost a platform rebuild and change
  # nothing.
  VITIS_APP_qspi_SRC             := examples/qspi sw/lib/qspi

  # Empty Application(C) again — this one is pure
  # computation over memory and needs nothing from the BSP but xil_printf.
  #
  # The BSP has no SHA-256 to borrow. A search of the generated platform turns
  # up only PolarSSL's SHA-*1* inside lwIP's PPP subsystem, which is the wrong
  # algorithm and compiled out of this configuration anyway; and Zynq-7000 has
  # no crypto accelerator, since the CSU hardware SHA is an UltraScale+ part.
  # So it is software, written from FIPS 180-4, and the known-answer vectors on
  # the console are what make it trustworthy.
  VITIS_APP_sha256_SRC           := examples/sha256 sw/lib/sha256

  # The two halves of the MultiBoot redirect demo.
  #
  # Neither is a 'vitis-run' target and neither can be. MultiBoot is only
  # observable when the BootROM is actually fetching a boot image from flash;
  # with JP4 on JTAG it waits for a debugger instead, so a JTAG-loaded build of
  # these apps would report a meaningless MULTIBOOT_ADDR and then soft-reset
  # into nothing. They go into BOOT.BIN images (see BOOT_IMAGES below) and are
  # proven by booting the board, not by downloading them.
  #
  # multiboot   — slot A, at QSPI 0x000000: self-checks, then redirects.
  # multiboot_b — slot B, at QSPI 0x700000: reports and stops. Deliberately does
  #               not link multiboot.[ch]; not having multiboot_to() in scope is
  #               a stronger guarantee that the chain ends here than remembering
  #               not to call it.
  VITIS_APP_multiboot_SRC        := examples/multiboot sw/lib/multiboot
  VITIS_APP_multiboot_b_SRC      := examples/multiboot_b sw/lib/multiboot

  # The field updater -- the app that composes every library
  # module into the nine-step pipeline in doc/icd_updater.md. One binary
  # serves both roles: in the golden image it IS the updater, in the update
  # image it proves the payload healthy and commits. It learns which from the
  # MultiBoot slot index at runtime, so there is no per-image build.
  #
  # Built from the echo template for the same reason the tftp_client example was: the
  # network bring-up (xemac_add, the SCU-timer platform layer, the RTL8211E
  # link path) costs a session to get right and is already right there.
  # sw/lib/ctrl is the operator interface -- doc/icd_control_channel.md. It
  # replaces the serial 'u' trigger, which was provisional: the deployment has
  # Ethernet and no console. sw/lib/netpump came out of sw/lib/tftp when this
  # became its second caller; it drives etharp_tmr() by hand under NO_SYS=1 and
  # there must be exactly one copy of it in a binary.
  # ── FAULT INJECTION — REMOVE AFTER THE RUN ────────────────
  # Uncommenting this builds an updater that comes up in the UPDATE role, proves
  # its payload healthy, and then deliberately hangs before committing. The
  # watchdog resets it, the arbiter counts 1 -> 2 -> 3, and at BOOT_ATTEMPTS_MAX
  # the board falls back to golden. That is the entire safety argument of Case B
  # and it is the one link in the chain that has never been executed.
  #
  # Safe to build with: the hook sits inside run_update(), so the SAME binary in
  # the golden image still serves normally -- and golden is not reflashed for
  # this anyway. The fault image reaches the board the way a real update does,
  # over TFTP, which is the whole point of testing it this way.
  #
  # The declared list is authoritative (mk/make/vivado.mk), so commenting this
  # line out really does remove the symbol on the next build.
  # VITIS_APP_updater_DEFINES    := UPDATER_FAULT_HANG=1
  VITIS_APP_updater_TEMPLATE     := lwIP Echo Server
  VITIS_APP_updater_SRC          := sw/arty_updater \
                                    sw/lib/ctrl \
                                    sw/lib/netpump \
                                    sw/lib/tftp \
                                    sw/lib/sha256 \
                                    sw/lib/qspi \
                                    sw/lib/multiboot \
                                    sw/lib/bootstate

  # lwip211 v1.6 in 2021.2, confirmed with 'repo -libs'. A BSP library belongs
  # to the platform, so adding this means regenerating: rm -rf build/vitis_ws,
  # then vitis-platform and vitis-apps again. The template may pull lwip211 in
  # by itself; declaring it is what makes the dependency explicit and survives
  # a future app that uses the stack from an Empty Application — this project's
  # TFTP client, if it ends up hand-written on the raw UDP API.
  VITIS_LIBS            := lwip211

  # The boot arbiter, overlaid onto the generated FSBL and compiled
  # into it, so the same hooked FSBL sits inside both the golden and the update
  # BOOT.BIN. Contract: doc/icd_boot_arbiter.md.
  #
  # Directories, with the framework discovering the files in them. The library
  # modules come along because the arbiter reuses them rather than re-deriving
  # them -- re-derivation is exactly how the earlier slot-role defect reached four
  # artefacts before any of it ran.
  #
  # This is why sw/lib/ exists at all: an example directory holds an app's
  # main.c, and the bootloader already has an entry point. The updater was the
  # first consumer that wanted a driver without its demo app, which is the
  # moment the driver stopped being app code.
  VITIS_BOOT_SRC        := sw/arty_fsbl_hooks \
                           sw/lib/bootstate \
                           sw/lib/qspi \
                           sw/lib/multiboot

  # Autodetect is broken for this board's Realtek PHY. get_Realtek_phy_speed()
  # in the BSP waits for autonegotiation (which completes), then requires bit 10
  # of the PHY's register 17 before it will report a speed — and on this PHY
  # that bit never comes up, so it returns XST_FAILURE and init_emacps gives up
  # with "Phy setup error". Reproducible on every run and on the driver's own
  # retry, so it is not a settling race.
  #
  # Pinning the speed takes the other branch of phy_setup_emacps, which calls
  # configure_IEEE_phy_speed() and never reads that bit. Autonegotiation still
  # runs — the fixed-speed path restricts what the PHY advertises rather than
  # forcing the link — so 1000BASE-T stays standards-compliant.
  # 1000 got past the error but then flapped: every link-up immediately went
  # down again, forever. Two candidate causes, and 100 Mbps distinguishes them:
  #
  #   a) gigabit RGMII timing is not usable on this board, in which case 100
  #      links and holds;
  #   b) fixed-speed mode is structurally unstable in this driver — the link
  #      monitor calls phy_setup_emacps() on every transition to up, which
  #      reconfigures the PHY and restarts negotiation, and the path only
  #      sleeps 1 s afterwards. If that is it, 100 flaps identically and the
  #      answer is autodetect plus a fix to the Realtek speed read.
  VITIS_BSP_CONFIG      := phy_link_speed=CONFIG_LINKSPEED100

  # Which app 'make vitis-run' downloads. Switch to regs, lwip_echo or
  # tftp_client to re-run the regs, lwip_echo or tftp_client examples — each stays a standalone ELF on
  # purpose.
  VITIS_RUN_APP         := sha256

  # ── Boot images (MultiBoot redirect demo) ──────────────────────────────────────
  # The first non-volatile artefacts this project has ever produced. Built by
  # 'make DESIGN=ps boot-image' and written by 'make DESIGN=ps flash-boot'; the
  # framework side lives in mk/make/vivado.mk § boot images.
  #
  # These are the demo pair, deliberately named apart from boot/arty/golden.bif
  # and update.bif, which describe the REAL images the updater design builds and which
  # this demo does not touch.
  #
  # The offsets are the flash map, and 0x700000 / 0x8000 = 0xE0 is
  # exactly the number slot B prints back as proof of where it was fetched from.
  #
  # FLASH WITH JP4 IN JTAG MODE, then move it to QSPI to run the demo.
  # program_flash drives the QSPI through a helper FSBL it downloads over JTAG;
  # if the board is set to boot from the flash being written, it is booting out
  # of the memory under the pen. Moving JP4 back to JTAG is also the complete
  # recovery path if anything here goes wrong.
  # The REAL pair. Same flash map as the MultiBoot demo above,
  # which is the point -- that demo proved the map and the redirect with two
  # trivial images before these two existed.
  #
  # golden is flashed once, over JTAG, and never again. update is NOT normally
  # flashed at all: it is served over TFTP as update.bin and written by the
  # updater, which verifies it before and after programming. Flashing it
  # directly is a bring-up shortcut, not the mechanism under test -- and doing
  # so bypasses the very step that makes an update safe.
  #
  # FLASH WITH JP4 IN JTAG MODE, then move it to QSPI to run. Moving JP4 back
  # to JTAG is the complete, unconditional recovery path.

  # bootgen's device family. No framework default on purpose: bootgen accepts a
  # wrong -arch, exits 0, and writes an image the BootROM will not load.
  BOOT_ARCH                := zynq
  BOOT_IMAGES              := mb_golden mb_update golden update
  BOOT_mb_golden_BIF       := boot/arty/mb_golden.bif
  BOOT_mb_golden_OFFSET    := 0x000000
  BOOT_mb_update_BIF       := boot/arty/mb_update.bif
  BOOT_mb_update_OFFSET    := 0x700000
  BOOT_golden_BIF          := boot/arty/golden.bif
  BOOT_golden_OFFSET       := 0x000000
  BOOT_update_BIF          := boot/arty/update.bif
  BOOT_update_OFFSET       := 0x700000

  # ONLY golden is ever written by the programmer.
  #
  # update.bin is built here and then served over TFTP, because being installed
  # by the updater -- fetched, hashed, written, read back, hashed again, and only
  # then marked present -- IS the thing this design exists to demonstrate. Flashing
  # it directly would skip every one of those steps and prove nothing.
  #
  # The mb_* pair is the MultiBoot demo. It stays buildable as a regression
  # check on the redirect mechanism, but writing it now would put a console-only
  # image at offset 0 where the real golden belongs.
  BOOT_FLASH_IMAGES        := golden

  VIVADO_IP_axi_pc_0_VLNV   := xilinx.com:ip:axi_protocol_converter:2.1
  VIVADO_IP_axi_pc_0_CONFIG := CONFIG.SI_PROTOCOL=AXI3 CONFIG.MI_PROTOCOL=AXI4LITE CONFIG.ID_WIDTH=12 CONFIG.TRANSLATION_MODE=2
else
  $(error DESIGN must be 'bench' or 'ps', got '$(DESIGN)')
endif

# Arty Z7 target device — verified against Vivado 2021.2: get_parts returns it.
VIVADO_PART_arty   := xc7z020clg400-1
# Board selection. Only the Arty Z7-20 is wired up here; the indirection stays
# so another board is one VIVADO_PART_<name> line plus its constraints.
BOARD       ?= arty
VIVADO_PART := $(VIVADO_PART_$(BOARD))

# ── GCC (bare-metal C) ───────────────────────────────────────────
# Everything under sw/ is Arty Z7 = Zynq-7000 = Cortex-A9 (the FSBL and the
# bare-metal updater both run on the APU). NOT Cortex-R5 — the Zynq-7000 has no
# such core; that is an UltraScale+ part, and it would need no
# cross-compiler here at all. The A9 in Zynq-7000 has NEON + VFPv3, hence
# -mfpu=neon; Cortex-R5 has VFPv3-D16 and no NEON, so the previous
# '-mcpu=cortex-r5 -mfpu=neon' pair was not a valid target in the first place.
CC       := arm-none-eabi-gcc
AR       := arm-none-eabi-ar
CFLAGS   := -Wall -Wextra -O2 -g -mcpu=cortex-a9 -mfloat-abi=hard -mfpu=neon
INC_DIRS := sw/arty_fsbl_hooks sw/arty_updater

# ── .gitignore (generated files to exclude) ───────────────────────────────────
# Generated by scan_project.sh — do not commit
# **/Makefile.mk
