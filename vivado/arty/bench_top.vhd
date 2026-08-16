-- bench_top.vhd — PS-less hardware bench top, Arty Z7-20
--
-- The smallest thing that can prove blinkctl on silicon:
--
--     sys_clk (H16, 125 MHz) ─┬─ POR ─ aresetn
--                             │
--     JTAG-to-AXI master ─────┴─ blinkctl_axil ─ led_o[3:0]  (R14/P14/N16/M14)
--
-- No PS7, no DDR, no MIO, no board file, no IP Integrator. blinkctl is an
-- AXI4-Lite slave and the bench only ever needs one master; JTAG-to-AXI is a
-- master in its own right and reaches the fabric straight off the JTAG cable,
-- so the CPU is dead weight for this purpose. The Arty Z7 carries a 125 MHz
-- PL oscillator on H16 (board file: sys_clk), which is the same frequency the
-- ICD specifies for FCLK_CLK0 — so blinkctl runs at its designed rate and the
-- v1/v2 divider constants stay meaningful.
--
-- THIS IS NOT THE SHIPPED DESIGN. The updater loads the PL bitstream out of
-- BOOT.BIN via the FSBL, which is a PS design by definition — that one is
-- vivado/arty/build_bd.tcl. This top exists for fast RTL bring-up: it builds
-- in a fraction of the time and drops almost all of the fabric that belongs
-- to the interconnect and the PS rather than to blinkctl.
--
-- No address decoder is present: the JTAG master's AXI port drives blinkctl
-- directly. That is not a limitation here — blinkctl decodes addr(5 downto 2)
-- only and ignores everything above, so the guide's TC replay commands work
-- unchanged at their ICD addresses:
--
--   create_hw_axi_txn wr [get_hw_axis hw_axi_1] -address 0x43C00010 \
--       -data 0xCAFEF00D -type write
--
-- Reset: there is no reset pin on the board to use, so a power-on reset is
-- generated locally. blinkctl loads its register file from a synchronous
-- reset, so it must see rst asserted after configuration or the reset values
-- in the ICD never land.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity bench_top is
  generic (
    G_VERSION       : std_logic_vector(31 downto 0) := x"00010001";
    G_BLINK_DIV_RST : natural                       := 125_000_000;
    -- POR length in clocks. 256 is far longer than anything downstream needs
    -- and still finishes in ~2 us at 125 MHz.
    G_POR_CYCLES    : positive                      := 256
  );
  port (
    sys_clk : in  std_logic;
    led_o   : out std_logic_vector(3 downto 0)
  );
end entity bench_top;

architecture rtl of bench_top is

  component jtag_axi_0 is
    port (
      aclk          : in  std_logic;
      aresetn       : in  std_logic;
      m_axi_awaddr  : out std_logic_vector(31 downto 0);
      m_axi_awprot  : out std_logic_vector(2 downto 0);
      m_axi_awvalid : out std_logic;
      m_axi_awready : in  std_logic;
      m_axi_wdata   : out std_logic_vector(31 downto 0);
      m_axi_wstrb   : out std_logic_vector(3 downto 0);
      m_axi_wvalid  : out std_logic;
      m_axi_wready  : in  std_logic;
      m_axi_bresp   : in  std_logic_vector(1 downto 0);
      m_axi_bvalid  : in  std_logic;
      m_axi_bready  : out std_logic;
      m_axi_araddr  : out std_logic_vector(31 downto 0);
      m_axi_arprot  : out std_logic_vector(2 downto 0);
      m_axi_arvalid : out std_logic;
      m_axi_arready : in  std_logic;
      m_axi_rdata   : in  std_logic_vector(31 downto 0);
      m_axi_rresp   : in  std_logic_vector(1 downto 0);
      m_axi_rvalid  : in  std_logic;
      m_axi_rready  : out std_logic
      );
  end component jtag_axi_0;

  signal aresetn   : std_logic := '0';
  signal por_count : unsigned(15 downto 0) := (others => '0');

  signal awaddr  : std_logic_vector(31 downto 0);
  signal awprot  : std_logic_vector(2 downto 0);
  signal awvalid : std_logic;
  signal awready : std_logic;
  signal wdata   : std_logic_vector(31 downto 0);
  signal wstrb   : std_logic_vector(3 downto 0);
  signal wvalid  : std_logic;
  signal wready  : std_logic;
  signal bresp   : std_logic_vector(1 downto 0);
  signal bvalid  : std_logic;
  signal bready  : std_logic;
  signal araddr  : std_logic_vector(31 downto 0);
  signal arprot  : std_logic_vector(2 downto 0);
  signal arvalid : std_logic;
  signal arready : std_logic;
  signal rdata   : std_logic_vector(31 downto 0);
  signal rresp   : std_logic_vector(1 downto 0);
  signal rvalid  : std_logic;
  signal rready  : std_logic;

begin

  -- ── Power-on reset ──────────────────────────────────────────────────────────
  -- Counter saturates once and never rolls back, so aresetn rises exactly once
  -- after configuration and stays high.
  POR_PROC : process (sys_clk)
  begin
    if rising_edge(sys_clk) then
      if por_count < G_POR_CYCLES then
        por_count <= por_count + 1;
        aresetn   <= '0';
      else
        aresetn <= '1';
      end if;
    end if;
  end process POR_PROC;

  JTAG_MASTER : jtag_axi_0
    port map (
      aclk          => sys_clk,
      aresetn       => aresetn,
      m_axi_awaddr  => awaddr,
      m_axi_awprot  => awprot,
      m_axi_awvalid => awvalid,
      m_axi_awready => awready,
      m_axi_wdata   => wdata,
      m_axi_wstrb   => wstrb,
      m_axi_wvalid  => wvalid,
      m_axi_wready  => wready,
      m_axi_bresp   => bresp,
      m_axi_bvalid  => bvalid,
      m_axi_bready  => bready,
      m_axi_araddr  => araddr,
      m_axi_arprot  => arprot,
      m_axi_arvalid => arvalid,
      m_axi_arready => arready,
      m_axi_rdata   => rdata,
      m_axi_rresp   => rresp,
      m_axi_rvalid  => rvalid,
      m_axi_rready  => rready
      );

  DUT : entity work.blinkctl_axil
    generic map (
      G_VERSION       => G_VERSION,
      G_BLINK_DIV_RST => G_BLINK_DIV_RST
      )
    port map (
      s_axi_aclk    => sys_clk,
      s_axi_aresetn => aresetn,
      s_axi_awaddr  => awaddr,
      s_axi_awprot  => awprot,
      s_axi_awvalid => awvalid,
      s_axi_awready => awready,
      s_axi_wdata   => wdata,
      s_axi_wstrb   => wstrb,
      s_axi_wvalid  => wvalid,
      s_axi_wready  => wready,
      s_axi_bresp   => bresp,
      s_axi_bvalid  => bvalid,
      s_axi_bready  => bready,
      s_axi_araddr  => araddr,
      s_axi_arprot  => arprot,
      s_axi_arvalid => arvalid,
      s_axi_arready => arready,
      s_axi_rdata   => rdata,
      s_axi_rresp   => rresp,
      s_axi_rvalid  => rvalid,
      s_axi_rready  => rready,
      led_o         => led_o
      );

end architecture rtl;
