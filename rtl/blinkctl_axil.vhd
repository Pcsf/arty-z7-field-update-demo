-- blinkctl_axil.vhd — flat AXI4-Lite wrapper around blinkctl, for IP Integrator
--
-- blinkctl's bus ports are VHDL records (blinkctl_axil_in_type /
-- blinkctl_axil_out_type). Records keep the core readable and are ideal in
-- simulation, but IP Integrator cannot infer an AXI interface from them and a
-- record has no meaning at a device pin. This wrapper is the thin, purely
-- structural adapter that lets the SAME core drop into a block design:
--
--   * flattens the records into the s_axi_* signal names Vivado infers as an
--     aximm slave (the X_INTERFACE attributes below make it explicit rather
--     than relying on name inference);
--   * inverts AXI's active-LOW s_axi_aresetn into blinkctl's active-HIGH rst.
--     That polarity flip is the one place this file can be wrong in a way
--     that still elaborates, which is why tb_blinkctl_axil.vhd tests it.
--
-- No logic lives here. Every register, every response and the whole ICD
-- contract stay in blinkctl.vhd, which is the authority on behaviour.
--
-- Address decode note: blinkctl decodes addr(5 downto 2) only — a 64-byte
-- window. Upper address bits are ignored, so the core aliases every 64 bytes
-- inside whatever range the address editor assigns it (4K minimum in IPI).
-- Reads/writes above offset 0x17 within that window are discard/read-as-zero
-- per ICD clause 1.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.blinkctl_pkg.all;

entity blinkctl_axil is
  -- Generic TYPES differ from the core's on purpose. blinkctl declares
  -- G_BLINK_DIV_RST as unsigned(31 downto 0) with a to_unsigned() default,
  -- which is the natural VHDL choice but which IP Integrator cannot infer:
  --   ERROR [IP_Flow 19-627] Unsupported function call or array usage
  --   "to_unsigned" found in expression.
  -- The wrapper therefore takes a plain natural and converts below, so the
  -- parameter shows up as an ordinary integer in the BD customisation GUI.
  generic (
    G_VERSION       : std_logic_vector(31 downto 0) := x"00010001";
    G_BLINK_DIV_RST : natural                       := 125_000_000
  );
  port (
    s_axi_aclk    : in  std_logic;
    s_axi_aresetn : in  std_logic;

    s_axi_awaddr  : in  std_logic_vector(31 downto 0);
    s_axi_awprot  : in  std_logic_vector(2 downto 0);
    s_axi_awvalid : in  std_logic;
    s_axi_awready : out std_logic;

    s_axi_wdata   : in  std_logic_vector(31 downto 0);
    s_axi_wstrb   : in  std_logic_vector(3 downto 0);
    s_axi_wvalid  : in  std_logic;
    s_axi_wready  : out std_logic;

    s_axi_bresp   : out std_logic_vector(1 downto 0);
    s_axi_bvalid  : out std_logic;
    s_axi_bready  : in  std_logic;

    s_axi_araddr  : in  std_logic_vector(31 downto 0);
    s_axi_arprot  : in  std_logic_vector(2 downto 0);
    s_axi_arvalid : in  std_logic;
    s_axi_arready : out std_logic;

    s_axi_rdata   : out std_logic_vector(31 downto 0);
    s_axi_rresp   : out std_logic_vector(1 downto 0);
    s_axi_rvalid  : out std_logic;
    s_axi_rready  : in  std_logic;

    led_o         : out std_logic_vector(3 downto 0)
  );

  -- IP Integrator interface inference. Vivado would guess most of this from
  -- the s_axi_* naming convention, but stating it removes the guesswork —
  -- particularly ASSOCIATED_BUSIF (which clock drives the interface) and the
  -- ACTIVE_LOW reset polarity.
  --
  -- These specifications must sit in the ENTITY declarative part: an
  -- attribute specification has to appear in the same immediate scope as the
  -- item it names, and ports are declared here, not in the architecture.
  attribute X_INTERFACE_INFO      : string;
  attribute X_INTERFACE_PARAMETER : string;

  attribute X_INTERFACE_INFO of s_axi_aclk    : signal is "xilinx.com:signal:clock:1.0 S_AXI_CLK CLK";
  attribute X_INTERFACE_PARAMETER of s_axi_aclk : signal is
    "XIL_INTERFACENAME S_AXI_CLK, ASSOCIATED_BUSIF S_AXI, ASSOCIATED_RESET s_axi_aresetn";
  attribute X_INTERFACE_INFO of s_axi_aresetn : signal is "xilinx.com:signal:reset:1.0 S_AXI_RST RST";
  attribute X_INTERFACE_PARAMETER of s_axi_aresetn : signal is
    "XIL_INTERFACENAME S_AXI_RST, POLARITY ACTIVE_LOW";

  attribute X_INTERFACE_INFO of s_axi_awaddr  : signal is "xilinx.com:interface:aximm:1.0 S_AXI AWADDR";
  attribute X_INTERFACE_INFO of s_axi_awprot  : signal is "xilinx.com:interface:aximm:1.0 S_AXI AWPROT";
  attribute X_INTERFACE_INFO of s_axi_awvalid : signal is "xilinx.com:interface:aximm:1.0 S_AXI AWVALID";
  attribute X_INTERFACE_INFO of s_axi_awready : signal is "xilinx.com:interface:aximm:1.0 S_AXI AWREADY";
  attribute X_INTERFACE_INFO of s_axi_wdata   : signal is "xilinx.com:interface:aximm:1.0 S_AXI WDATA";
  attribute X_INTERFACE_INFO of s_axi_wstrb   : signal is "xilinx.com:interface:aximm:1.0 S_AXI WSTRB";
  attribute X_INTERFACE_INFO of s_axi_wvalid  : signal is "xilinx.com:interface:aximm:1.0 S_AXI WVALID";
  attribute X_INTERFACE_INFO of s_axi_wready  : signal is "xilinx.com:interface:aximm:1.0 S_AXI WREADY";
  attribute X_INTERFACE_INFO of s_axi_bresp   : signal is "xilinx.com:interface:aximm:1.0 S_AXI BRESP";
  attribute X_INTERFACE_INFO of s_axi_bvalid  : signal is "xilinx.com:interface:aximm:1.0 S_AXI BVALID";
  attribute X_INTERFACE_INFO of s_axi_bready  : signal is "xilinx.com:interface:aximm:1.0 S_AXI BREADY";
  attribute X_INTERFACE_INFO of s_axi_araddr  : signal is "xilinx.com:interface:aximm:1.0 S_AXI ARADDR";
  attribute X_INTERFACE_INFO of s_axi_arprot  : signal is "xilinx.com:interface:aximm:1.0 S_AXI ARPROT";
  attribute X_INTERFACE_INFO of s_axi_arvalid : signal is "xilinx.com:interface:aximm:1.0 S_AXI ARVALID";
  attribute X_INTERFACE_INFO of s_axi_arready : signal is "xilinx.com:interface:aximm:1.0 S_AXI ARREADY";
  attribute X_INTERFACE_INFO of s_axi_rdata   : signal is "xilinx.com:interface:aximm:1.0 S_AXI RDATA";
  attribute X_INTERFACE_INFO of s_axi_rresp   : signal is "xilinx.com:interface:aximm:1.0 S_AXI RRESP";
  attribute X_INTERFACE_INFO of s_axi_rvalid  : signal is "xilinx.com:interface:aximm:1.0 S_AXI RVALID";
  attribute X_INTERFACE_INFO of s_axi_rready  : signal is "xilinx.com:interface:aximm:1.0 S_AXI RREADY";

end entity blinkctl_axil;

architecture rtl of blinkctl_axil is

  signal axi_i : blinkctl_axil_in_type;
  signal axi_o : blinkctl_axil_out_type;
  signal rst   : std_logic;

begin

  -- AXI reset is active-low; blinkctl's is active-high.
  rst <= not s_axi_aresetn;

  axi_i.awaddr  <= s_axi_awaddr;
  axi_i.awprot  <= s_axi_awprot;
  axi_i.awvalid <= s_axi_awvalid;
  axi_i.wdata   <= s_axi_wdata;
  axi_i.wstrb   <= s_axi_wstrb;
  axi_i.wvalid  <= s_axi_wvalid;
  axi_i.bready  <= s_axi_bready;
  axi_i.araddr  <= s_axi_araddr;
  axi_i.arprot  <= s_axi_arprot;
  axi_i.arvalid <= s_axi_arvalid;
  axi_i.rready  <= s_axi_rready;

  s_axi_awready <= axi_o.awready;
  s_axi_wready  <= axi_o.wready;
  s_axi_bvalid  <= axi_o.bvalid;
  s_axi_bresp   <= axi_o.bresp;
  s_axi_arready <= axi_o.arready;
  s_axi_rvalid  <= axi_o.rvalid;
  s_axi_rdata   <= axi_o.rdata;
  s_axi_rresp   <= axi_o.rresp;

  CORE : entity work.blinkctl(two_process)
    generic map (
      G_VERSION       => G_VERSION,
      G_BLINK_DIV_RST => to_unsigned(G_BLINK_DIV_RST, 32)
      )
    port map (
      clk   => s_axi_aclk,
      rst   => rst,
      axi_i => axi_i,
      axi_o => axi_o,
      led_o => led_o
      );

end architecture rtl;
