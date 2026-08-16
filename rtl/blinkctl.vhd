-- blinkctl.vhd — AXI4-Lite LED blink controller (two-process VHDL-2008)
--
-- Full AXI4-Lite slave, non-pipelined: one outstanding write and one
-- outstanding read. AW and W are accepted independently (any order, or the
-- same cycle); the write executes once both halves are captured. WSTRB and
-- AWPROT/ARPROT are ignored (full-word writes per the ICD). All responses
-- are OKAY; unmapped offsets are write-discard / read-as-zero.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.blinkctl_pkg.all;

entity blinkctl is
  generic (
    G_VERSION       : std_logic_vector(31 downto 0) := x"00010001";
    G_BLINK_DIV_RST : unsigned(31 downto 0) := to_unsigned(125_000_000, 32)
  );
  port (
    clk   : in  std_logic;
    rst   : in  std_logic;
    axi_i : in  blinkctl_axil_in_type;
    axi_o : out blinkctl_axil_out_type;
    led_o : out std_logic_vector(3 downto 0)
  );
end entity blinkctl;

architecture two_process of blinkctl is
  type reg_type is record
    divider     : unsigned(31 downto 0);
    div_pending : unsigned(31 downto 0);
    heartbeat   : unsigned(31 downto 0);
    ctrl        : unsigned(31 downto 0);
    scratch     : unsigned(31 downto 0);
    status      : unsigned(31 downto 0);
    led_state   : std_logic;
    -- write channels
    awready : std_logic;
    aw_hold : std_logic;
    awaddr  : std_logic_vector(31 downto 0);
    wready  : std_logic;
    w_hold  : std_logic;
    wdata   : std_logic_vector(31 downto 0);
    bvalid  : std_logic;
    bresp   : std_logic_vector(1 downto 0);
    -- read channels
    arready : std_logic;
    rvalid  : std_logic;
    rdata   : std_logic_vector(31 downto 0);
    rresp   : std_logic_vector(1 downto 0);
  end record;

  function clamp_div(v : unsigned(31 downto 0)) return unsigned is
  begin
    if v = 0 then
      return to_unsigned(1, 32);
    end if;
    return v;
  end function;

  function reset_regs return reg_type is
    variable d : unsigned(31 downto 0);
    variable r : reg_type;
  begin
    d := clamp_div(G_BLINK_DIV_RST);
    r.divider     := d;
    r.div_pending := d;
    r.heartbeat   := R_HEARTBEAT_RESET;
    r.ctrl        := R_CTRL_RESET;
    r.scratch     := R_SCRATCH_RESET;
    r.status      := R_STATUS_RESET;
    r.led_state   := '0';
    r.awready     := '0';
    r.aw_hold     := '0';
    r.awaddr      := (others => '0');
    r.wready      := '0';
    r.w_hold      := '0';
    r.wdata       := (others => '0');
    r.bvalid      := '0';
    r.bresp       := "00";
    r.arready     := '0';
    r.rvalid      := '0';
    r.rdata       := (others => '0');
    r.rresp       := "00";
    return r;
  end function;

  signal r, rin : reg_type := reset_regs;

  function read_reg(addr : std_logic_vector(31 downto 0); s : reg_type)
    return std_logic_vector is
  begin
    case to_integer(unsigned(addr(5 downto 2))) is
      when 0 => return G_VERSION;
      when 1 => return std_logic_vector(s.ctrl);
      when 2 => return std_logic_vector(s.div_pending);
      when 3 => return std_logic_vector(s.heartbeat);
      when 4 => return std_logic_vector(s.scratch);
      when 5 => return std_logic_vector(s.status);
      when others => return (31 downto 0 => '0');
    end case;
  end function;
begin
  comb : process(all)
    variable v  : reg_type;
    variable wd : unsigned(31 downto 0);
  begin
    v := r;

    v.heartbeat := r.heartbeat + 1;

    if r.ctrl(0) = '1' then
      if r.divider <= 1 then
        v.divider   := r.div_pending;
        v.led_state := not r.led_state;
      else
        v.divider := r.divider - 1;
      end if;
    end if;

    -- Write address/data channels: accept independently, in any order.
    if r.awready = '1' and axi_i.awvalid = '1' then
      v.awaddr  := axi_i.awaddr;
      v.aw_hold := '1';
    end if;
    if r.wready = '1' and axi_i.wvalid = '1' then
      v.wdata  := axi_i.wdata;
      v.w_hold := '1';
    end if;

    -- Write response completion.
    if r.bvalid = '1' and axi_i.bready = '1' then
      v.bvalid := '0';
    end if;

    -- Execute the write once both halves are captured (possibly both this
    -- cycle). WSTRB ignored: full-word writes per the ICD.
    if v.aw_hold = '1' and v.w_hold = '1' and v.bvalid = '0' then
      wd := unsigned(v.wdata);
      case to_integer(unsigned(v.awaddr(5 downto 2))) is
        when 0 => null; -- VERSION is read-only; write accepted/discarded.
        when 1 => v.ctrl := wd;
        when 2 =>
          v.div_pending := clamp_div(wd);
          if wd = 0 then
            v.status(0) := '1';
          end if;
        when 3 => null; -- HEARTBEAT is read-only.
        when 4 => v.scratch := wd;
        when 5 => v.status := r.status and not wd; -- STATUS W1C.
        when others => null;
      end case;
      v.aw_hold := '0';
      v.w_hold  := '0';
      v.bvalid  := '1';
      v.bresp   := "00";
    end if;

    -- Read channels: respond from registers at the AR handshake, hold RDATA
    -- until the master takes it.
    if r.rvalid = '1' and axi_i.rready = '1' then
      v.rvalid := '0';
    end if;
    if r.arready = '1' and axi_i.arvalid = '1' then
      v.rdata  := read_reg(axi_i.araddr, r);
      v.rresp  := "00";
      v.rvalid := '1';
    end if;

    -- Ready generation (registered, non-pipelined): a channel is ready
    -- whenever it is not holding a beat and no response is outstanding.
    v.awready := not v.aw_hold and not v.bvalid;
    v.wready  := not v.w_hold and not v.bvalid;
    v.arready := not v.rvalid;

    if rst = '1' then
      v := reset_regs;
    end if;

    rin <= v;

    -- update outputs
    axi_o.awready <= r.awready;
    axi_o.wready  <= r.wready;
    axi_o.bvalid  <= r.bvalid;
    axi_o.bresp   <= r.bresp;
    axi_o.arready <= r.arready;
    axi_o.rvalid  <= r.rvalid;
    axi_o.rdata   <= r.rdata;
    axi_o.rresp   <= r.rresp;
    led_o         <= "000" & r.led_state;
  end process comb;

  regs : process(clk)
  begin
    if rising_edge(clk) then
      r <= rin;
    end if;
  end process regs;
end architecture two_process;
