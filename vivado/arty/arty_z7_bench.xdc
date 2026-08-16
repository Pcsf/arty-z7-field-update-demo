# ==============================================================================
# arty_z7_bench.xdc — constraints for the PS-less bench design (bench_top)
#
# Separate from arty_z7.xdc because the two designs have different port lists:
# the block-design build clocks blinkctl from the PS7's FCLK_CLK0 and has no
# clock port at all, while this one takes the board's PL oscillator as a
# top-level input and therefore needs both a pin and a create_clock.
#
# Pins from vivado/board_files/arty-z7-20/A.0/part0_pins.xml:
#   sys_clk = H16 (125 MHz PL oscillator)
#   LD0..3  = R14, P14, N16, M14                       all LVCMOS33
# ==============================================================================

set_property -dict { PACKAGE_PIN H16  IOSTANDARD LVCMOS33 } [get_ports { sys_clk }]
create_clock -period 8.000 -name sys_clk -waveform {0.000 4.000} [get_ports { sys_clk }]

set_property -dict { PACKAGE_PIN R14  IOSTANDARD LVCMOS33 } [get_ports { led_o[0] }]
set_property -dict { PACKAGE_PIN P14  IOSTANDARD LVCMOS33 } [get_ports { led_o[1] }]
set_property -dict { PACKAGE_PIN N16  IOSTANDARD LVCMOS33 } [get_ports { led_o[2] }]
set_property -dict { PACKAGE_PIN M14  IOSTANDARD LVCMOS33 } [get_ports { led_o[3] }]
