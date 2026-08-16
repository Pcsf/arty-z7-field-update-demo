-- tb_blinkctl.vhd — Self-checking testbench for blinkctl
--
-- Implements TC-01..TC-11. The contract tripwire monitors (tb_tripwires.vhd)
-- watch the same interface for the whole run.
--
-- Protocol: full AXI4-Lite (independent AW/W/B/AR/R channels, all ready
-- signals). TC-09 holds AW/W valid continuously across two writes so the
-- slave's registered readys must back-pressure without losing a beat;
-- TC-10 launches a read and a write in the same cycle and requires both to
-- complete independently.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use std.env.all;
use work.blinkctl_pkg.all;
use work.axil_bfm.all;

entity tb_blinkctl is
end entity tb_blinkctl;

architecture sim of tb_blinkctl is
  constant C_CLK_PERIOD : time    := 8 ns;
  constant BASE_ADDR    : integer := 16#43C0_0000#;
  constant C_DIV_TB     : natural := 8;  -- G_BLINK_DIV_RST for simulation

  signal clk : std_logic := '0';
  signal rst : std_logic := '1';
  signal axi_i : blinkctl_axil_in_type := (
    awaddr => (others => '0'), awprot => (others => '0'), awvalid => '0',
    wdata  => (others => '0'), wstrb => (others => '0'), wvalid => '0',
    bready => '0',
    araddr => (others => '0'), arprot => (others => '0'), arvalid => '0',
    rready => '0');
  signal axi_o : blinkctl_axil_out_type;
  signal led_o : std_logic_vector(3 downto 0);

  signal awaddr  : std_logic_vector(31 downto 0) := (others => '0');
  signal awvalid : std_logic                     := '0';
  signal wdata   : std_logic_vector(31 downto 0) := (others => '0');
  signal wstrb   : std_logic_vector(3 downto 0)  := (others => '0');
  signal wvalid  : std_logic                     := '0';
  signal bready  : std_logic                     := '0';
  signal araddr  : std_logic_vector(31 downto 0) := (others => '0');
  signal arvalid : std_logic                     := '0';
  signal rready  : std_logic                     := '0';

  -- Wait for the next LED edge and timestamp it; toggled = false on timeout.
  procedure wait_led_toggle(
    signal led       : in  std_logic;
    constant tmax    : in  time;
    variable t_edge  : out time;
    variable toggled : out boolean
    ) is
    variable v : std_logic;
  begin
    v       := led;
    wait until led /= v for tmax;
    toggled := led /= v;
    t_edge  := now;
  end procedure;
begin
  clk <= not clk after C_CLK_PERIOD/2;

  axi_i.awaddr  <= awaddr;
  axi_i.awvalid <= awvalid;
  axi_i.wdata   <= wdata;
  axi_i.wstrb   <= wstrb;
  axi_i.wvalid  <= wvalid;
  axi_i.bready  <= bready;
  axi_i.araddr  <= araddr;
  axi_i.arvalid <= arvalid;
  axi_i.rready  <= rready;

  DUT : entity work.blinkctl(two_process)
    generic map (
      G_VERSION       => x"00010001",
      G_BLINK_DIV_RST => to_unsigned(C_DIV_TB, 32)
      )
    port map (
      clk   => clk,
      rst   => rst,
      axi_i => axi_i,
      axi_o => axi_o,
      led_o => led_o
      );

  TRIPWIRES : entity work.tb_tripwires
    generic map (
      G_BLINK_DIV_RST     => to_unsigned(C_DIV_TB, 32),
      G_CTRL_RST          => R_CTRL_RESET(0),
      G_RESP_TIMEOUT      => 16,
      G_HALT_ON_VIOLATION => true
      )
    port map (
      clk     => clk,
      rst     => rst,
      awaddr  => axi_i.awaddr,
      awvalid => axi_i.awvalid,
      awready => axi_o.awready,
      wdata   => axi_i.wdata,
      wvalid  => axi_i.wvalid,
      wready  => axi_o.wready,
      bvalid  => axi_o.bvalid,
      arvalid => axi_i.arvalid,
      rvalid  => axi_o.rvalid,
      led0    => led_o(0)
      );

  TB_PROC : process
    variable tc_passed                          : integer := 0;
    variable tc_total                           : integer := 0;
    variable ok1, ok2, ok3, ok4, ok5            : boolean;
    variable d0, d1, d2                         : unsigned(31 downto 0);
    variable t0, t1, t2                         : time;
    variable tg0, tg1, tg2                      : boolean;
    variable led_frz                            : std_logic_vector(3 downto 0);
    variable bcount, aw_cnt, w_cnt              : integer;
    variable bv_prev                            : std_logic;
    variable ar_done, aw_done, w_done           : boolean;
    variable r_done, b_done                     : boolean;

    procedure tc_check(constant name : in string; constant cond : in boolean) is
    begin
      tc_total := tc_total + 1;
      if cond then
        report name & " PASS";
        tc_passed := tc_passed + 1;
      else
        report name & " FAIL" severity error;
      end if;
    end procedure;
  begin
    rst <= '1';
    wait for 100 ns;
    wait until rising_edge(clk);
    rst <= '0';

    -- TC-01: after reset, VERSION reads G_VERSION and LED[0] = '0'.
    ok1 := led_o = "0000";  -- sampled at reset release, before any bus traffic
    axil_read(clk, araddr, arvalid, axi_o.arready, axi_o.rdata, axi_o.rvalid, axi_o.rresp, rready,
              BASE_ADDR + R_VERSION_OFFSET, d0, ok2);
    tc_check("TC-01", ok1 and ok2 and d0 = x"00010001");

    -- TC-02: SCRATCH write/read roundtrip returns the written value.
    axil_write(clk, awaddr, awvalid, axi_o.awready, wdata, wstrb, wvalid, axi_o.wready,
               bready, axi_o.bvalid, axi_o.bresp,
               BASE_ADDR + R_SCRATCH_OFFSET, x"CAFEF00D", ok1);
    axil_read(clk, araddr, arvalid, axi_o.arready, axi_o.rdata, axi_o.rvalid, axi_o.rresp, rready,
              BASE_ADDR + R_SCRATCH_OFFSET, d0, ok2);
    tc_check("TC-02", ok1 and ok2 and d0 = x"CAFEF00D");

    -- TC-03: HEARTBEAT reads 100 cycles apart differ by 100 +/-0. The BFM has
    -- a fixed read latency, so two back-to-back reads measure the intrinsic
    -- sample distance; inserting exactly 100 clocks must add exactly 100.
    axil_read(clk, araddr, arvalid, axi_o.arready, axi_o.rdata, axi_o.rvalid, axi_o.rresp, rready,
              BASE_ADDR + R_HEARTBEAT_OFFSET, d0, ok1);
    axil_read(clk, araddr, arvalid, axi_o.arready, axi_o.rdata, axi_o.rvalid, axi_o.rresp, rready,
              BASE_ADDR + R_HEARTBEAT_OFFSET, d1, ok2);
    for i in 1 to 100 loop
      wait until rising_edge(clk);
    end loop;
    axil_read(clk, araddr, arvalid, axi_o.arready, axi_o.rdata, axi_o.rvalid, axi_o.rresp, rready,
              BASE_ADDR + R_HEARTBEAT_OFFSET, d2, ok3);
    tc_check("TC-03", ok1 and ok2 and ok3 and (d2 - d1) = (d1 - d0) + 100);

    -- TC-04: with BLINK_DIV = 8 (the TB reset default), LED[0] toggles every
    -- 8 clk cycles exactly.
    wait_led_toggle(led_o(0), 1 us, t0, tg0);
    wait_led_toggle(led_o(0), 1 us, t1, tg1);
    tc_check("TC-04", tg0 and tg1 and (t1 - t0) = C_DIV_TB * C_CLK_PERIOD);

    -- TC-05: BLINK_DIV written mid-count takes effect only at the next
    -- zero-crossing. Set a long half-period (24), then write 6 mid-count
    -- (the write itself takes ~4 cycles << 24): the running half-period must
    -- complete at the OLD length, the following one at the NEW length.
    axil_write(clk, awaddr, awvalid, axi_o.awready, wdata, wstrb, wvalid, axi_o.wready,
               bready, axi_o.bvalid, axi_o.bresp,
               BASE_ADDR + R_BLINK_DIV_OFFSET, to_unsigned(24, 32), ok1);
    wait_led_toggle(led_o(0), 1 us, t0, tg0);  -- old period may still finish here
    wait_led_toggle(led_o(0), 1 us, t0, tg1);  -- from this toggle the period is 24
    axil_write(clk, awaddr, awvalid, axi_o.awready, wdata, wstrb, wvalid, axi_o.wready,
               bready, axi_o.bvalid, axi_o.bresp,
               BASE_ADDR + R_BLINK_DIV_OFFSET, to_unsigned(6, 32), ok2);
    wait_led_toggle(led_o(0), 1 us, t1, tg2);
    tc_check("TC-05", ok1 and ok2 and tg0 and tg1 and tg2
             and (t1 - t0) = 24 * C_CLK_PERIOD);
    wait_led_toggle(led_o(0), 1 us, t2, tg0);
    tc_check("TC-05b", tg0 and (t2 - t1) = 6 * C_CLK_PERIOD);

    -- TC-06: CTRL.BLINK_EN = 0 freezes LED at its current level (does not
    -- force it off); re-enabling resumes blinking.
    axil_write(clk, awaddr, awvalid, axi_o.awready, wdata, wstrb, wvalid, axi_o.wready,
               bready, axi_o.bvalid, axi_o.bresp,
               BASE_ADDR + R_CTRL_OFFSET, to_unsigned(0, 32), ok1);
    led_frz := led_o;
    wait for 20 * C_CLK_PERIOD;  -- > 3 half-periods at the current divider (6)
    ok2     := led_o = led_frz;
    axil_write(clk, awaddr, awvalid, axi_o.awready, wdata, wstrb, wvalid, axi_o.wready,
               bready, axi_o.bvalid, axi_o.bresp,
               BASE_ADDR + R_CTRL_OFFSET, to_unsigned(1, 32), ok3);
    wait_led_toggle(led_o(0), 30 * C_CLK_PERIOD, t0, tg0);
    tc_check("TC-06", ok1 and ok2 and ok3 and tg0);

    -- TC-07: write to VERSION completes with OKAY (the BFM asserts BRESP
    -- internally) and VERSION is unchanged.
    axil_write(clk, awaddr, awvalid, axi_o.awready, wdata, wstrb, wvalid, axi_o.wready,
               bready, axi_o.bvalid, axi_o.bresp,
               BASE_ADDR + R_VERSION_OFFSET, x"FFFFFFFF", ok1);
    axil_read(clk, araddr, arvalid, axi_o.arready, axi_o.rdata, axi_o.rvalid, axi_o.rresp, rready,
              BASE_ADDR + R_VERSION_OFFSET, d0, ok2);
    tc_check("TC-07", ok1 and ok2 and d0 = x"00010001");

    -- TC-08: BLINK_DIV = 0 is clamped to 1 and STATUS.DIVCLAMP sets; W1C
    -- clears it.
    axil_write(clk, awaddr, awvalid, axi_o.awready, wdata, wstrb, wvalid, axi_o.wready,
               bready, axi_o.bvalid, axi_o.bresp,
               BASE_ADDR + R_BLINK_DIV_OFFSET, to_unsigned(0, 32), ok1);
    axil_read(clk, araddr, arvalid, axi_o.arready, axi_o.rdata, axi_o.rvalid, axi_o.rresp, rready,
              BASE_ADDR + R_BLINK_DIV_OFFSET, d0, ok2);
    axil_read(clk, araddr, arvalid, axi_o.arready, axi_o.rdata, axi_o.rvalid, axi_o.rresp, rready,
              BASE_ADDR + R_STATUS_OFFSET, d1, ok3);
    axil_write(clk, awaddr, awvalid, axi_o.awready, wdata, wstrb, wvalid, axi_o.wready,
               bready, axi_o.bvalid, axi_o.bresp,
               BASE_ADDR + R_STATUS_OFFSET, to_unsigned(1, 32), ok4);
    axil_read(clk, araddr, arvalid, axi_o.arready, axi_o.rdata, axi_o.rvalid, axi_o.rresp, rready,
              BASE_ADDR + R_STATUS_OFFSET, d2, ok5);
    tc_check("TC-08", ok1 and ok2 and ok3 and ok4 and ok5
             and d0 = 1 and d1(0) = '1' and d2 = 0);
    axil_write(clk, awaddr, awvalid, axi_o.awready, wdata, wstrb, wvalid, axi_o.wready,
               bready, axi_o.bvalid, axi_o.bresp,
               BASE_ADDR + R_BLINK_DIV_OFFSET, to_unsigned(C_DIV_TB, 32), ok1);

    -- TC-09 (bad weather): back-to-back writes with no gaps lose no
    -- transactions. AW/W valid are held continuously across two writes; the
    -- slave's registered readys must back-pressure the second beat until the
    -- first B handshake completes. Expect two AW and two W handshakes, two
    -- BVALID pulses, and the second value in SCRATCH.
    wait until rising_edge(clk);
    awaddr  <= std_logic_vector(to_unsigned(BASE_ADDR + R_SCRATCH_OFFSET, 32));
    wdata   <= x"11111111";
    wstrb   <= "1111";
    awvalid <= '1';
    wvalid  <= '1';
    bready  <= '1';
    aw_cnt  := 0;
    w_cnt   := 0;
    bcount  := 0;
    bv_prev := '0';
    for i in 1 to 16 loop
      wait until rising_edge(clk);
      if aw_cnt < 2 and axi_o.awready = '1' then
        aw_cnt := aw_cnt + 1;
        if aw_cnt = 2 then
          awvalid <= '0';
        end if;
      end if;
      if w_cnt < 2 and axi_o.wready = '1' then
        w_cnt := w_cnt + 1;
        if w_cnt = 1 then
          wdata <= x"22222222";
        else
          wvalid <= '0';
        end if;
      end if;
      if axi_o.bvalid = '1' and bv_prev = '0' then
        bcount := bcount + 1;
      end if;
      bv_prev := axi_o.bvalid;
    end loop;
    bready <= '0';
    axil_read(clk, araddr, arvalid, axi_o.arready, axi_o.rdata, axi_o.rvalid, axi_o.rresp, rready,
              BASE_ADDR + R_SCRATCH_OFFSET, d0, ok1);
    tc_check("TC-09", ok1 and aw_cnt = 2 and w_cnt = 2 and bcount = 2
             and d0 = x"22222222");

    -- TC-10 (bad weather): read and write channels active simultaneously do
    -- not deadlock. Launch a VERSION read and a SCRATCH write in the same
    -- cycle; every channel must handshake and both responses must arrive.
    wait until rising_edge(clk);
    araddr  <= std_logic_vector(to_unsigned(BASE_ADDR + R_VERSION_OFFSET, 32));
    arvalid <= '1';
    rready  <= '1';
    awaddr  <= std_logic_vector(to_unsigned(BASE_ADDR + R_SCRATCH_OFFSET, 32));
    wdata   <= x"5A5A5A5A";
    wstrb   <= "1111";
    awvalid <= '1';
    wvalid  <= '1';
    bready  <= '1';
    ar_done := false;
    aw_done := false;
    w_done  := false;
    r_done  := false;
    b_done  := false;
    ok1     := false;
    for i in 1 to 16 loop
      wait until rising_edge(clk);
      if not ar_done and axi_o.arready = '1' then
        ar_done := true;
        arvalid <= '0';
      end if;
      if not aw_done and axi_o.awready = '1' then
        aw_done := true;
        awvalid <= '0';
      end if;
      if not w_done and axi_o.wready = '1' then
        w_done := true;
        wvalid <= '0';
      end if;
      if not r_done and axi_o.rvalid = '1' then
        r_done := true;
        ok1    := axi_o.rdata = x"00010001";
      end if;
      if axi_o.bvalid = '1' then
        b_done := true;
      end if;
    end loop;
    rready <= '0';
    bready <= '0';
    ok2    := ar_done and aw_done and w_done and r_done and b_done;
    axil_read(clk, araddr, arvalid, axi_o.arready, axi_o.rdata, axi_o.rvalid, axi_o.rresp, rready,
              BASE_ADDR + R_SCRATCH_OFFSET, d0, ok3);
    tc_check("TC-10", ok1 and ok2 and ok3 and d0 = x"5A5A5A5A");

    -- TC-11: mid-run reset returns every register to its documented reset
    -- value. Dirty the state first.
    axil_write(clk, awaddr, awvalid, axi_o.awready, wdata, wstrb, wvalid, axi_o.wready,
               bready, axi_o.bvalid, axi_o.bresp,
               BASE_ADDR + R_SCRATCH_OFFSET, x"13371337", ok1);
    axil_write(clk, awaddr, awvalid, axi_o.awready, wdata, wstrb, wvalid, axi_o.wready,
               bready, axi_o.bvalid, axi_o.bresp,
               BASE_ADDR + R_CTRL_OFFSET, to_unsigned(0, 32), ok2);
    wait until rising_edge(clk);
    rst <= '1';
    for i in 1 to 5 loop
      wait until rising_edge(clk);
    end loop;
    rst <= '0';
    ok3 := led_o = "0000";
    axil_read(clk, araddr, arvalid, axi_o.arready, axi_o.rdata, axi_o.rvalid, axi_o.rresp, rready,
              BASE_ADDR + R_SCRATCH_OFFSET, d0, ok4);
    axil_read(clk, araddr, arvalid, axi_o.arready, axi_o.rdata, axi_o.rvalid, axi_o.rresp, rready,
              BASE_ADDR + R_CTRL_OFFSET, d1, ok5);
    axil_read(clk, araddr, arvalid, axi_o.arready, axi_o.rdata, axi_o.rvalid, axi_o.rresp, rready,
              BASE_ADDR + R_BLINK_DIV_OFFSET, d2, ok1);
    tc_check("TC-11", ok1 and ok2 and ok3 and ok4 and ok5
             and d0 = R_SCRATCH_RESET and d1 = R_CTRL_RESET
             and d2 = C_DIV_TB);

    report "========================================";
    report "TC passed: " & integer'image(tc_passed) & " / " & integer'image(tc_total);
    report "========================================";
    assert tc_passed = tc_total report "TEST FAILED" severity failure;
    report "TEST COMPLETE";
    finish;
    wait;
  end process TB_PROC;
end architecture sim;
