// qomu_scheme: 512x8 RAM wrapper for the open (ql_symbiflow) toolchain.
// Same interface as the SpDE-generated r512x8_512x8 used by the USB endpoint
// buffers; implemented on the toolchain's RAM_8K_BLK hard macro. The SpDE-only
// select / power-control inputs (WD_SEL, RD_SEL, LS, DS, SD, ...) are unused.
module r512x8_512x8 (WA,RA,WD,WD_SEL,RD_SEL,WClk,RClk,WClk_En,RClk_En,WEN,RD,LS,DS,SD,LS_RB1,DS_RB1,SD_RB1);

input  [8:0] WA;
input  [8:0] RA;
input        WD_SEL,RD_SEL;
input        WClk,RClk;
input        WClk_En,RClk_En;
input        WEN;
input  [7:0] WD;
input        LS,DS,SD,LS_RB1,DS_RB1,SD_RB1;
output [7:0] RD;

parameter addr_int       = 9;
parameter data_depth_int = 512;
parameter data_width_int = 8;
parameter wr_enable_int  = 1;
parameter reg_rd_int     = 0;

RAM_8K_BLK #(
    .addr_int       ( addr_int       ),
    .data_depth_int ( data_depth_int ),
    .data_width_int ( data_width_int ),
    .wr_enable_int  ( wr_enable_int  ),
    .reg_rd_int     ( reg_rd_int     )
) RAM_INST (
    .WA      ( WA      ),
    .RA      ( RA      ),
    .WD      ( WD      ),
    .WClk    ( WClk    ),
    .RClk    ( RClk    ),
    .WClk_En ( WClk_En ),
    .RClk_En ( RClk_En ),
    .WEN     ( WEN     ),
    .RD      ( RD      )
);

endmodule
