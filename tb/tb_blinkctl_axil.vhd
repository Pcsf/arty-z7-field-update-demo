-- tb_blinkctl_axil.vhd — testbench for the IPI wrapper, not for the core
--
-- tb_blinkctl.vhd is the authority on blinkctl's behaviour and covers
-- TC-01..TC-11 against the record interface. This testbench covers only what
-- blinkctl_axil.vhd ADDS on top of that core, since a structural wrapper can
-- be wrong in exactly three ways and still elaborate cleanly:
--
--   WC-01  reset polarity  — AXI aresetn is active-LOW, blinkctl's rst is
--                            active-HIGH. Inverted wrongly, the core either
--                            never leaves reset or never enters it. Checked
--                            both directions: held in reset the LED stays
--                            dark and registers read their reset values;
--                            released, the heartbeat advances.
--   WC-02  channel mapping — a swapped or dropped signal in the flatten.
--                            Exercised by a full write/read round trip on
--                            SCRATCH and a VERSION read (RO path).
--   WC-03  end-to-end life — LED toggles at the programmed divider, proving
--                            clock, reset and the register file all reached
--                            the core through the wrapper.
--
-- Deliberately NOT re-tested here: response codes, W1C on STATUS, the
-- mid-count divider contract, back-pressure. Those are core behaviour and
-- belong to tb_blinkctl.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use std.env.all;
use work.blinkctl_pkg.all;
use work.axil_bfm.all;

entity tb_blinkctl_axil is
end entity tb_blinkctl_axil;

architecture sim of tb_blinkctl_axil is
  constant C_CLK_PERIOD : time    := 8 ns;   -- 125 MHz, the Arty Z7 FCLK_CLK0
  constant BASE_ADDR    : integer := 16#43C0_0000#;
  constant C_DIV_TB     : natural := 8;

  signal clk     : std_logic := '0';
  signal aresetn : std_logic := '0';         -- active LOW: start asserted

  signal awaddr  : std_logic_vector(31 downto 0) := (others => '0');
  signal awvalid : std_logic                     := '0';
  signal awready : std_logic;
  signal wdata   : std_logic_vector(31 downto 0) := (others => '0');
  signal wstrb   : std_logic_vector(3 downto 0)  := (others => '0');
  signal wvalid  : std_logic                     := '0';
  signal wready  : std_logic;
  signal bresp   : std_logic_vector(1 downto 0);
  signal bvalid  : std_logic;
  signal bready  : std_logic                     := '0';
  signal araddr  : std_logic_vector(31 downto 0) := (others => '0');
  signal arvalid : std_logic                     := '0';
  signal arready : std_logic;
  signal rdata   : std_logic_vector(31 downto 0);
  signal rresp   : std_logic_vector(1 downto 0);
  signal rvalid  : std_logic;
  signal rready  : std_logic                     := '0';
  signal led_o   : std_logic_vector(3 downto 0);

  signal wc_passed : integer := 0;
  signal wc_total  : integer := 0;

  procedure wc_check(constant name : in string; constant ok : in boolean) is
  begin
    if ok then
      report name & " PASS";
    else
      report name & " FAIL" severity error;
    end if;
  end procedure;
begin
  clk <= not clk after C_CLK_PERIOD/2;

  DUT : entity work.blinkctl_axil
    generic map (
      G_VERSION       => x"00010001",
      G_BLINK_DIV_RST => C_DIV_TB
      )
    port map (
      s_axi_aclk    => clk,
      s_axi_aresetn => aresetn,
      s_axi_awaddr  => awaddr,
      s_axi_awprot  => (others => '0'),
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
      s_axi_arprot  => (others => '0'),
      s_axi_arvalid => arvalid,
      s_axi_arready => arready,
      s_axi_rdata   => rdata,
      s_axi_rresp   => rresp,
      s_axi_rvalid  => rvalid,
      s_axi_rready  => rready,
      led_o         => led_o
      );

  TB_PROC : process
    variable d      : unsigned(31 downto 0);
    variable ok1    : boolean;
    variable ok2    : boolean;
    variable hb1    : unsigned(31 downto 0);
    variable hb2    : unsigned(31 downto 0);
    variable passed : integer := 0;
    variable total  : integer := 0;
    variable v_led   : std_logic;
    variable toggled : boolean;

    procedure tally(constant name : in string; constant ok : in boolean) is
    begin
      total := total + 1;
      if ok then
        passed := passed + 1;
      end if;
      wc_check(name, ok);
    end procedure;
  begin
    -- ── WC-01a: held in reset (aresetn low) the core must stay dark ────────
    aresetn <= '0';
    for i in 1 to 20 loop
      wait until rising_edge(clk);
    end loop;
    tally("WC-01a reset asserted -> LED dark", led_o = "0000");

    -- Release reset. If the polarity were inverted, the core would now be
    -- held IN reset and every check below would fail.
    aresetn <= '1';
    for i in 1 to 5 loop
      wait until rising_edge(clk);
    end loop;

    -- ── WC-02a: VERSION reads back through the wrapper (RO path) ──────────
    axil_read(clk, araddr, arvalid, arready, rdata, rvalid, rresp, rready,
              BASE_ADDR + R_VERSION_OFFSET, d, ok1);
    tally("WC-02a VERSION readback", ok1 and d = unsigned'(x"00010001"));

    -- ── WC-02b: SCRATCH round trip (R/W path, both directions) ────────────
    axil_read(clk, araddr, arvalid, arready, rdata, rvalid, rresp, rready,
              BASE_ADDR + R_SCRATCH_OFFSET, d, ok1);
    tally("WC-02b SCRATCH reset value", ok1 and d = R_SCRATCH_RESET);

    axil_write(clk, awaddr, awvalid, awready, wdata, wstrb, wvalid, wready,
               bready, bvalid, bresp,
               BASE_ADDR + R_SCRATCH_OFFSET, x"CAFEF00D", ok1);
    axil_read(clk, araddr, arvalid, arready, rdata, rvalid, rresp, rready,
              BASE_ADDR + R_SCRATCH_OFFSET, d, ok2);
    tally("WC-02c SCRATCH write/read round trip",
          ok1 and ok2 and d = unsigned'(x"CAFEF00D"));

    -- ── WC-01b: reset actually reaches the core ───────────────────────────
    -- SCRATCH now holds CAFEF00D. Pulse aresetn low; it must return to its
    -- reset value. This is the check that fails if 'rst' were tied off.
    aresetn <= '0';
    for i in 1 to 5 loop
      wait until rising_edge(clk);
    end loop;
    aresetn <= '1';
    for i in 1 to 5 loop
      wait until rising_edge(clk);
    end loop;
    axil_read(clk, araddr, arvalid, arready, rdata, rvalid, rresp, rready,
              BASE_ADDR + R_SCRATCH_OFFSET, d, ok1);
    tally("WC-01b reset pulse restores SCRATCH", ok1 and d = R_SCRATCH_RESET);

    -- ── WC-03a: HEARTBEAT advances (clock reaches the core) ───────────────
    axil_read(clk, araddr, arvalid, arready, rdata, rvalid, rresp, rready,
              BASE_ADDR + R_HEARTBEAT_OFFSET, hb1, ok1);
    axil_read(clk, araddr, arvalid, arready, rdata, rvalid, rresp, rready,
              BASE_ADDR + R_HEARTBEAT_OFFSET, hb2, ok2);
    tally("WC-03a HEARTBEAT advances", ok1 and ok2 and hb2 > hb1);

    -- ── WC-03b: LED toggles at the reset divider ──────────────────────────
    -- G_BLINK_DIV_RST = 8 half-period cycles, so a toggle must appear well
    -- inside 4x that. Timeout rather than hang if the LED is stuck.
    v_led := led_o(0);
    wait until led_o(0) /= v_led for C_CLK_PERIOD * C_DIV_TB * 4;
    toggled := led_o(0) /= v_led;
    tally("WC-03b LED toggles at reset divider", toggled);

    report "========================================";
    report "WC passed: " & integer'image(passed) & " / " & integer'image(total);
    report "========================================";
    assert passed = total report "WRAPPER TEST FAILED" severity failure;
    report "WRAPPER TEST COMPLETE";
    finish;
    wait;
  end process TB_PROC;
end architecture sim;
