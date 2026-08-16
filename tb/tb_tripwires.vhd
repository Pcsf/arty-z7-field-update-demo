-- tb_tripwires.vhd — Independent watch-only monitors for blinkctl ICD clauses.
-- Instantiated by tb_blinkctl alongside the DUT; fires only on violation.
--
-- The monitors observe the DUT interface only. The divider contract is
-- enforced with a shadow model built from the committed register writes and
-- led_o: every LED half-period must span exactly the number of enabled clk
-- cycles given by the divider value that was pending at the *previous*
-- toggle. This single check covers both ICD clauses "BLINK_DIV never updates
-- mid-count" and "LED toggles exactly every BLINK_DIV cycles when enabled"
-- (a mid-count reload that changes the period shows up as a wrong interval;
-- one that doesn't is unobservable and harmless by definition).
--
-- Each tripwire must be proven alive by injecting a fault in the RTL that makes
-- it fire; a monitor that has never fired is not known to work.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.blinkctl_pkg.all;

entity tb_tripwires is
  generic (
    G_BLINK_DIV_RST : unsigned(31 downto 0);
    G_CTRL_RST      : std_logic := '1';
    G_RESP_TIMEOUT  : natural   := 16;
    -- false in the TDD RED run: report violations as errors and keep going,
    -- so a dead-bus stub yields one informative failure per test case.
    G_HALT_ON_VIOLATION : boolean := true
    );
  port (
    clk     : in std_logic;
    rst     : in std_logic;
    awaddr  : in std_logic_vector(31 downto 0);
    awvalid : in std_logic;
    awready : in std_logic;
    wdata   : in std_logic_vector(31 downto 0);
    wvalid  : in std_logic;
    wready  : in std_logic;
    bvalid  : in std_logic;
    arvalid : in std_logic;
    rvalid  : in std_logic;
    led0    : in std_logic
    );
end entity tb_tripwires;

architecture sim of tb_tripwires is
  -- Mirrors the ICD clamp rule: BLINK_DIV = 0 is illegal, hardware clamps to 1.
  function clamp_div(v : unsigned(31 downto 0)) return unsigned is
  begin
    if v = 0 then
      return to_unsigned(1, 32);
    end if;
    return v;
  end function;

  procedure violation(constant msg : in string) is
  begin
    if G_HALT_ON_VIOLATION then
      assert false report msg severity failure;
    else
      report msg severity error;
    end if;
  end procedure;
begin

  -- Tripwire 1: presented transactions always complete (ICD: no bus hang).
  -- A stuck ready also trips this, since the response can never follow.
  p_contract_bus_liveness : process(clk)
    variable wtimer : integer := -1;
    variable rtimer : integer := -1;
  begin
    if rising_edge(clk) then
      if rst = '1' then
        wtimer := -1;
        rtimer := -1;
      else
        if bvalid = '1' then
          wtimer := -1;
        elsif awvalid = '1' and wvalid = '1' and wtimer < 0 then
          wtimer := G_RESP_TIMEOUT;
        elsif wtimer = 0 then
          violation("ICD VIOLATION [bus liveness]: write presented, no BVALID within "
                    & integer'image(G_RESP_TIMEOUT) & " cycles");
          wtimer := -1;  -- disarm so a non-halting run reports once per request
        elsif wtimer > 0 then
          wtimer := wtimer - 1;
        end if;

        if rvalid = '1' then
          rtimer := -1;
        elsif arvalid = '1' and rtimer < 0 then
          rtimer := G_RESP_TIMEOUT;
        elsif rtimer = 0 then
          violation("ICD VIOLATION [bus liveness]: read presented, no RVALID within "
                    & integer'image(G_RESP_TIMEOUT) & " cycles");
          rtimer := -1;  -- disarm so a non-halting run reports once per request
        elsif rtimer > 0 then
          rtimer := rtimer - 1;
        end if;
      end if;
    end if;
  end process;

  -- Tripwires 2+3: BLINK_DIV loads only at the zero-crossing, and the LED
  -- toggles exactly every BLINK_DIV enabled cycles.
  --
  -- Everything this monitor samples (led0, the bus) is one cycle behind the
  -- DUT's registers, so the shadow of the committed writes is delayed by one
  -- extra cycle (pending_d/en_d) to stay exactly aligned with the observed
  -- led0. The check is exact: no tolerance, including across a CTRL freeze.
  p_contract_div : process(clk)
    variable pending   : unsigned(31 downto 0)          := clamp_div(G_BLINK_DIV_RST);
    variable pending_d : unsigned(31 downto 0)          := clamp_div(G_BLINK_DIV_RST);
    variable en        : std_logic                      := G_CTRL_RST;
    variable en_d      : std_logic                      := G_CTRL_RST;
    variable active    : unsigned(31 downto 0)          := clamp_div(G_BLINK_DIV_RST);
    variable cnt       : natural                        := 0;
    variable prev_led  : std_logic                      := '0';
    variable aw_seen   : boolean                        := false;
    variable w_seen    : boolean                        := false;
    variable addr_q    : natural                        := 0;
    variable data_q    : std_logic_vector(31 downto 0)  := (others => '0');
  begin
    if rising_edge(clk) then
      if rst = '1' then
        pending   := clamp_div(G_BLINK_DIV_RST);
        pending_d := pending;
        en        := G_CTRL_RST;
        en_d      := en;
        active    := pending;
        cnt       := 0;
        prev_led  := '0';
        aw_seen   := false;
        w_seen    := false;
      else
        -- 1. LED toggle: the interval that just ended must span exactly
        --    'active' enabled cycles; the divider may reload only here.
        if led0 /= prev_led then
          if cnt /= to_integer(active) then
            violation("ICD VIOLATION [BLINK_DIV/LED period]: half-period was "
                      & integer'image(cnt) & " enabled cycles, contract says "
                      & integer'image(to_integer(active)));
          end if;
          active := pending_d;
          cnt    := 0;
        end if;
        prev_led := led0;

        -- 2. Count enabled cycles.
        if en_d = '1' then
          cnt := cnt + 1;
        end if;

        -- 3. Promote last cycle's committed writes into the delayed shadow.
        pending_d := pending;
        en_d      := en;

        -- 4. Track the AW/W handshakes; the DUT commits the write on the
        --    edge the later of the two halves lands.
        if awvalid = '1' and awready = '1' then
          addr_q  := to_integer(unsigned(awaddr(5 downto 2)));
          aw_seen := true;
        end if;
        if wvalid = '1' and wready = '1' then
          data_q := wdata;
          w_seen := true;
        end if;
        if aw_seen and w_seen then
          case addr_q is
            when R_CTRL_OFFSET / 4      => en      := data_q(0);
            when R_BLINK_DIV_OFFSET / 4 => pending := clamp_div(unsigned(data_q));
            when others                 => null;
          end case;
          aw_seen := false;
          w_seen  := false;
        end if;
      end if;
    end if;
  end process;

end architecture sim;
