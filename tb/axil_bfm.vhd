-- axil_bfm.vhd — simple blocking AXI4-Lite BFM procedures
-- Full-protocol master: AW/W are presented together and retired
-- independently by AWREADY/WREADY; AR by ARREADY. Each valid is held until
-- its ready is sampled high at a rising edge (one transfer per handshake).
--
-- All waits carry a timeout that reports severity ERROR, releases the bus
-- and returns ok=false, so against a dead slave (the TDD RED stub) every
-- test case fails informatively instead of aborting the run. Fail-fast
-- enforcement of bus liveness is the tripwire monitor's job.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

package axil_bfm is
  procedure axil_write(
    signal clk     : in  std_logic;
    signal awaddr  : out std_logic_vector(31 downto 0);
    signal awvalid : out std_logic;
    signal awready : in  std_logic;
    signal wdata   : out std_logic_vector(31 downto 0);
    signal wstrb   : out std_logic_vector(3 downto 0);
    signal wvalid  : out std_logic;
    signal wready  : in  std_logic;
    signal bready  : out std_logic;
    signal bvalid  : in  std_logic;
    signal bresp   : in  std_logic_vector(1 downto 0);
    constant addr  : in  integer;
    constant data  : in  unsigned(31 downto 0);
    variable ok    : out boolean
  );

  procedure axil_read(
    signal clk     : in  std_logic;
    signal araddr  : out std_logic_vector(31 downto 0);
    signal arvalid : out std_logic;
    signal arready : in  std_logic;
    signal rdata   : in  std_logic_vector(31 downto 0);
    signal rvalid  : in  std_logic;
    signal rresp   : in  std_logic_vector(1 downto 0);
    signal rready  : out std_logic;
    constant addr  : in  integer;
    variable data  : out unsigned(31 downto 0);
    variable ok    : out boolean
  );
end package axil_bfm;

package body axil_bfm is
  constant C_TIMEOUT : integer := 100;

  procedure axil_write(
    signal clk     : in  std_logic;
    signal awaddr  : out std_logic_vector(31 downto 0);
    signal awvalid : out std_logic;
    signal awready : in  std_logic;
    signal wdata   : out std_logic_vector(31 downto 0);
    signal wstrb   : out std_logic_vector(3 downto 0);
    signal wvalid  : out std_logic;
    signal wready  : in  std_logic;
    signal bready  : out std_logic;
    signal bvalid  : in  std_logic;
    signal bresp   : in  std_logic_vector(1 downto 0);
    constant addr  : in  integer;
    constant data  : in  unsigned(31 downto 0);
    variable ok    : out boolean
  ) is
    variable waited  : integer := 0;
    variable aw_done : boolean := false;
    variable w_done  : boolean := false;
  begin
    ok := false;
    wait until rising_edge(clk);
    awaddr  <= std_logic_vector(to_unsigned(addr, 32));
    awvalid <= '1';
    wdata   <= std_logic_vector(data);
    wstrb   <= "1111";
    wvalid  <= '1';
    bready  <= '1';

    while not (aw_done and w_done) loop
      wait until rising_edge(clk);
      if not aw_done and awready = '1' then
        aw_done := true;
        awvalid <= '0';
      end if;
      if not w_done and wready = '1' then
        w_done := true;
        wvalid <= '0';
      end if;
      waited := waited + 1;
      if waited >= C_TIMEOUT then
        report "AXI write address/data timeout at 0x" & to_hstring(to_unsigned(addr, 32))
          severity error;
        awvalid <= '0';
        wvalid  <= '0';
        bready  <= '0';
        return;
      end if;
    end loop;

    while bvalid /= '1' loop
      wait until rising_edge(clk);
      waited := waited + 1;
      if waited >= C_TIMEOUT then
        report "AXI write response timeout at 0x" & to_hstring(to_unsigned(addr, 32))
          severity error;
        bready <= '0';
        return;
      end if;
    end loop;

    if bresp = "00" then
      ok := true;
    else
      report "AXI write to 0x" & to_hstring(to_unsigned(addr, 32))
        & " returned BRESP=" & to_string(bresp)
        severity error;
    end if;
    wait until rising_edge(clk);
    bready <= '0';
  end procedure axil_write;

  procedure axil_read(
    signal clk     : in  std_logic;
    signal araddr  : out std_logic_vector(31 downto 0);
    signal arvalid : out std_logic;
    signal arready : in  std_logic;
    signal rdata   : in  std_logic_vector(31 downto 0);
    signal rvalid  : in  std_logic;
    signal rresp   : in  std_logic_vector(1 downto 0);
    signal rready  : out std_logic;
    constant addr  : in  integer;
    variable data  : out unsigned(31 downto 0);
    variable ok    : out boolean
  ) is
    variable waited : integer := 0;
  begin
    ok   := false;
    data := (others => '0');
    wait until rising_edge(clk);
    araddr  <= std_logic_vector(to_unsigned(addr, 32));
    arvalid <= '1';
    rready  <= '1';

    while arready /= '1' loop
      wait until rising_edge(clk);
      waited := waited + 1;
      if waited >= C_TIMEOUT then
        report "AXI read address timeout at 0x" & to_hstring(to_unsigned(addr, 32))
          severity error;
        arvalid <= '0';
        rready  <= '0';
        return;
      end if;
    end loop;
    wait until rising_edge(clk);
    arvalid <= '0';

    while rvalid /= '1' loop
      wait until rising_edge(clk);
      waited := waited + 1;
      if waited >= C_TIMEOUT then
        report "AXI read data timeout at 0x" & to_hstring(to_unsigned(addr, 32))
          severity error;
        rready <= '0';
        return;
      end if;
    end loop;

    if rresp = "00" then
      data := unsigned(rdata);
      ok   := true;
    else
      report "AXI read from 0x" & to_hstring(to_unsigned(addr, 32))
        & " returned RRESP=" & to_string(rresp)
        severity error;
    end if;
    wait until rising_edge(clk);
    rready <= '0';
  end procedure axil_read;
end package body axil_bfm;
