include mk/Makefile

# ── print-<VAR>: read one make variable from a script ─────────────────────────
# host/release.sh asks for UPDATE_VERSION rather than carrying its own copy of
# the payload version (see project.mk § Payload version). Kept here rather than
# in mk/ because it is one line and the framework has no need of it; if a second
# project wants it, that is when it earns a place there.
.PHONY: print-%
print-%:
	@echo '$($*)'

# ── Preflight: are we actually inside the Vivado container? ───────────────────
# The Xilinx tools are not installed on the host — they live in the
# vivado-image:2021.2 container from xilinx-dev-env, which mounts this repo at
# /home/fpgauser/workspace. Running a vivado target on the host gets you
# "xvhdl: command not found" from a recipe several layers deep, which does not
# hint at the real cause. Say it up front instead. Warning, not error: clean,
# help, info and the ghdl flow must all still work on the host.
ifeq ($(TOOLCHAIN),vivado)
ifeq ($(shell command -v xvhdl 2>/dev/null),)
$(warning )
$(warning [PREFLIGHT] TOOLCHAIN is 'vivado' but the Xilinx tools are not on PATH.)
$(warning [PREFLIGHT] You are probably on the host. Start the container first:)
$(warning [PREFLIGHT]     ~/Documents/10-repos/11-gitRepo/xilinx-dev-env/vivado_docker.sh -c)
$(warning [PREFLIGHT] then re-run make from /home/fpgauser/workspace inside it.)
$(warning )
endif
endif

# ── Payload bitstreams ────────────────────────────────────────────────────────
# The boot images name build/boot/blinkctl_v1.bit and blinkctl_v2.bit -- the two
# payload versions the demo updates between. They are the SAME design built
# twice with a different G_VERSION and blink divider (project.mk § Payload
# version), so there is one bitstream target and two runs of it, each staged
# under its own name.
#
# Without this, 'boot-image' fails with "Cannot read BIT file" and the reason is
# not obvious: the design bitstream exists, just not under the names the .bif
# files ask for.
.PHONY: payload-bitstreams
payload-bitstreams:
	@test "$(DESIGN)" = "ps" || { \
	    echo "[PAYLOAD] run this with DESIGN=ps -- the boot images are PS designs."; \
	    exit 1; }
	@mkdir -p $(BOOT_DIR)
	@for v in v1 v2; do \
	    echo "[PAYLOAD] building $$v"; \
	    $(MAKE) --no-print-directory DESIGN=ps PAYLOAD=$$v bitstream || exit 1; \
	    cp -f "$(VIVADO_BIT)" "$(BOOT_DIR)/blinkctl_$$v.bit" || exit 1; \
	    echo "[PAYLOAD] staged $(BOOT_DIR)/blinkctl_$$v.bit"; \
	done

# ── Host tests ────────────────────────────────────────────────────────────────
# The manifest parser is the first thing on the board to touch bytes off the
# network, and every one of its failure modes is reachable from the wire. It
# runs here in a second; on the board it costs a workspace rebuild and a TFTP
# install. See test/manifest/README.org.
#
# LIVES OUTSIDE sw/ ON PURPOSE. Vitis imports an app's source directory
# RECURSIVELY, so a test/ subdirectory under sw/arty_updater/ is copied into the
# firmware project and compiled as part of it -- and the Vitis build reports
# that failure while still exiting zero. Host tests stay out of the embedded
# source tree.
.PHONY: manifest-test
manifest-test:
	@cc -std=c99 -Wall -Wextra \
	    -I test/manifest/stub -I sw/arty_updater \
	    -o $(CURDIR)/test/manifest/.test_manifest \
	    test/manifest/test_manifest.c
	@$(CURDIR)/test/manifest/.test_manifest
