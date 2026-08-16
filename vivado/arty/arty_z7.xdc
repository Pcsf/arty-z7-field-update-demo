# ==============================================================================
# arty_z7.xdc — Arty Z7-20 physical constraints for the PS design (ps_top)
#
# Only the four user LEDs are PL pins in this design. Everything else the
# design needs — the 125 MHz FCLK_CLK0 clock, DDR3 and the MIO peripherals —
# comes out of the PS7 block and is constrained by the Zynq board preset, not
# from this file. That is also why there is no create_clock here: FCLK_CLK0 is
# generated inside PS7 and Vivado derives its timing automatically.
#
# Pin locations and I/O standard are taken from Digilent's own board file,
# vivado/board_files/arty-z7-20/A.0/part0_pins.xml (leds_4bits_tri_o_0..3),
# not transcribed from a schematic by hand:
#
#     LD0 = R14   LD1 = P14   LD2 = N16   LD3 = M14      all LVCMOS33
#
# blinkctl drives led_o[0] as the blink output; led_o[3:1] follow the same
# register file and are wired so the whole nibble is visible on the bench.
# ==============================================================================

set_property -dict { PACKAGE_PIN R14  IOSTANDARD LVCMOS33 } [get_ports { led_o[0] }]
set_property -dict { PACKAGE_PIN P14  IOSTANDARD LVCMOS33 } [get_ports { led_o[1] }]
set_property -dict { PACKAGE_PIN N16  IOSTANDARD LVCMOS33 } [get_ports { led_o[2] }]
set_property -dict { PACKAGE_PIN M14  IOSTANDARD LVCMOS33 } [get_ports { led_o[3] }]
