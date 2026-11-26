# Timing constraints for the usb2serial gateware (VPR SDC subset).
# Sys_Clk1 = C21, the 48 MHz USB bit clock; Sys_Clk0 = C16, the 12 MHz
# protocol / Wishbone clock. Crossings between them go through the design's
# own synchronisers (strobe / dflip), so the domains are analysed separately.
#
# 72 MHz build (USB_CLK72 in AL4S3B_FPGA_Top.v): Sys_Clk1 = C21 = 36 MHz
# (27.778 ns), the receiver's 3x sample clock; constrained tighter so placement
# leaves margin. Judge results by the Sys_Clk1 CPD in route.log (must stay
# below 27.778 ns), not by the slack against this tighter period.
create_clock -period 25.0 Sys_Clk1
create_clock -period 83.333 Sys_Clk0
set_clock_groups -exclusive -group Sys_Clk1 -group Sys_Clk0
#
# 48 MHz build (USB_CLK72 undefined), instead of the three lines above (judge
# by the Sys_Clk1 CPD, which must stay below 20.833 ns):
#   create_clock -period 20.0 Sys_Clk1
#   create_clock -period 83.333 Sys_Clk0
#   set_clock_groups -exclusive -group Sys_Clk1 -group Sys_Clk0
