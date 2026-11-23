# Timing constraints for the usb2serial gateware (VPR SDC subset).
# Sys_Clk1 = C21, the 48 MHz USB bit clock; Sys_Clk0 = C16, the 12 MHz
# protocol / Wishbone clock. Crossings between them go through the design's
# own synchronisers (strobe / dflip), so the domains are analysed separately.
create_clock -period 20.833 Sys_Clk1
create_clock -period 83.333 Sys_Clk0
set_clock_groups -exclusive -group Sys_Clk1 -group Sys_Clk0
