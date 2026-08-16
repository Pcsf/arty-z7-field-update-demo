-- blinkctl_pkg.vhd — blinkctl package (simple AXI4-Lite records)

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

package blinkctl_pkg is
  constant C_AXI_DATA_WIDTH : integer := 32;
  constant C_AXI_ID_WIDTH   : integer := 1;

  -- Byte register offsets
  constant R_VERSION_OFFSET   : integer := 16#00#;
  constant R_CTRL_OFFSET      : integer := 16#04#;
  constant R_BLINK_DIV_OFFSET : integer := 16#08#;
  constant R_HEARTBEAT_OFFSET : integer := 16#0C#;
  constant R_SCRATCH_OFFSET   : integer := 16#10#;
  constant R_STATUS_OFFSET    : integer := 16#14#;

  constant R_VERSION_RESET   : unsigned(31 downto 0) := x"00010001";
  constant R_CTRL_RESET      : unsigned(31 downto 0) := x"00000001";
  constant R_BLINK_DIV_RESET : unsigned(31 downto 0) := to_unsigned(125_000_000, 32);
  constant R_HEARTBEAT_RESET : unsigned(31 downto 0) := (others => '0');
  constant R_SCRATCH_RESET   : unsigned(31 downto 0) := x"DEADBEEF";
  constant R_STATUS_RESET    : unsigned(31 downto 0) := (others => '0');

  -- Full AXI4-Lite channel set (single beat, 32-bit). AWPROT/ARPROT are
  -- carried for interface completeness and ignored by the slave; WSTRB is
  -- ignored (full-word writes per the ICD).
  type blinkctl_axil_in_type is record
    awaddr  : std_logic_vector(31 downto 0);
    awprot  : std_logic_vector(2 downto 0);
    awvalid : std_logic;
    wdata   : std_logic_vector(31 downto 0);
    wstrb   : std_logic_vector(3 downto 0);
    wvalid  : std_logic;
    bready  : std_logic;
    araddr  : std_logic_vector(31 downto 0);
    arprot  : std_logic_vector(2 downto 0);
    arvalid : std_logic;
    rready  : std_logic;
  end record;

  type blinkctl_axil_out_type is record
    awready : std_logic;
    wready  : std_logic;
    bvalid  : std_logic;
    bresp   : std_logic_vector(1 downto 0);
    arready : std_logic;
    rvalid  : std_logic;
    rdata   : std_logic_vector(31 downto 0);
    rresp   : std_logic_vector(1 downto 0);
  end record;
end package blinkctl_pkg;
