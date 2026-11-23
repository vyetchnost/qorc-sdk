// qomu_scheme: 512x8 synchronous FIFO for the open (ql_symbiflow) toolchain,
// same interface and flag encoding as the SpDE-generated f512x8_512x8 used by
// FPGA_FIFO (first-word-fall-through: DOUT shows the head entry, POP removes it).
//
// Default build: fabric pointers and flags around the RAM_8K_BLK wrapper
// r512x8_512x8, which is proven on hardware (USB endpoint buffers). The
// FIFO_8K_BLK hard-FIFO mapping did not move data on the board; it is kept
// behind `define USE_HARD_FIFO for reference.
module f512x8_512x8 (DIN,Fifo_Push_Flush,Fifo_Pop_Flush,PUSH,POP,Clk,
                     Clk_En,Fifo_Dir,Async_Flush,
                     Almost_Full,Almost_Empty,PUSH_FLAG,POP_FLAG,DOUT);

input        Fifo_Push_Flush,Fifo_Pop_Flush;
input        Clk;
input        PUSH,POP;
input  [7:0] DIN;
input        Clk_En,Fifo_Dir,Async_Flush;
output [7:0] DOUT;
output [3:0] PUSH_FLAG,POP_FLAG;
output       Almost_Full,Almost_Empty;

parameter data_depth_int = 512;
parameter data_width_int = 8;
parameter reg_rd_int     = 0;
parameter sync_fifo_int  = 1;

`ifdef USE_HARD_FIFO
FIFO_8K_BLK #(
    .data_depth_int ( data_depth_int ),
    .data_width_int ( data_width_int ),
    .reg_rd_int     ( reg_rd_int     ),
    .sync_fifo_int  ( sync_fifo_int  )
) FIFO_INST (
    .DIN             ( DIN             ),
    .PUSH            ( PUSH            ),
    .POP             ( POP             ),
    .Fifo_Push_Flush ( Fifo_Push_Flush ),
    .Fifo_Pop_Flush  ( Fifo_Pop_Flush  ),
    .Push_Clk        ( Clk             ),
    .Pop_Clk         ( Clk             ),
    .PUSH_FLAG       ( PUSH_FLAG       ),
    .POP_FLAG        ( POP_FLAG        ),
    .Push_Clk_En     ( Clk_En          ),
    .Pop_Clk_En      ( Clk_En          ),
    .Fifo_Dir        ( Fifo_Dir        ),
    .Async_Flush     ( Async_Flush     ),
    .Almost_Full     ( Almost_Full     ),
    .Almost_Empty    ( Almost_Empty    ),
    .DOUT            ( DOUT            )
);

`else
// 10-bit pointers: bit 9 tells full (count 512) from empty (count 0)
reg  [9:0] wptr = 0, rptr = 0;
wire [9:0] count = wptr - rptr;
wire [9:0] room  = 10'd512 - count;
wire       empty = (count == 10'd0);
wire       full  = count[9];

wire do_push = PUSH && !full && Clk_En;
wire do_pop  = POP && !empty && Clk_En;

// RAM read is synchronous: present the address of the head *after* this
// clock, so RD shows the new head in the next cycle.
wire [8:0] ra_next = do_pop ? (rptr[8:0] + 9'd1) : rptr[8:0];

always @(posedge Clk or posedge Async_Flush)
    if (Async_Flush) begin
        wptr <= 0;
        rptr <= 0;
    end else begin
        if (do_push || Fifo_Push_Flush) wptr <= Fifo_Push_Flush ? rptr : wptr + 10'd1;
        if (do_pop) rptr <= rptr + 10'd1;
    end

// write and read of the same address in one clock: RD would be stale,
// so forward the written byte for that one cycle
reg       byp = 0;
reg [7:0] byp_d = 0;
always @(posedge Clk or posedge Async_Flush)
    if (Async_Flush) begin
        byp <= 0;
        byp_d <= 0;
    end else begin
        byp <= do_push && (wptr[8:0] == ra_next);
        byp_d <= DIN;
    end

wire [7:0] ram_rd;
r512x8_512x8 fifo_ram (
    .WA      ( wptr[8:0] ),
    .RA      ( ra_next   ),
    .WD      ( DIN       ),
    .WD_SEL  ( 1'b1      ),
    .RD_SEL  ( 1'b1      ),
    .WClk    ( Clk       ),
    .RClk    ( Clk       ),
    .WClk_En ( 1'b1      ),
    .RClk_En ( 1'b1      ),
    .WEN     ( do_push   ),
    .RD      ( ram_rd    ),
    .LS      ( 1'b0      ),
    .DS      ( 1'b0      ),
    .SD      ( 1'b0      ),
    .LS_RB1  ( 1'b0      ),
    .DS_RB1  ( 1'b0      ),
    .SD_RB1  ( 1'b0      )
);

assign DOUT = byp ? byp_d : ram_rd;

// S3 FIFO flag encoding (see FPGA_FIFO.v)
reg [3:0] pop_flag, push_flag;
always @(*) begin
    if      (count == 0)   pop_flag = 4'h0;
    else if (count == 1)   pop_flag = 4'h1;
    else if (count < 4)    pop_flag = 4'h2;
    else if (count < 8)    pop_flag = 4'h3;
    else if (count < 16)   pop_flag = 4'h4;
    else if (count < 32)   pop_flag = 4'h5;
    else if (count < 64)   pop_flag = 4'h6;
    else if (count < 128)  pop_flag = 4'h8;
    else if (count < 256)  pop_flag = 4'hD;
    else if (count < 512)  pop_flag = 4'hE;
    else                   pop_flag = 4'hF;

    if      (full)         push_flag = 4'h0;
    else if (empty)        push_flag = 4'h1;
    else if (room > 256)   push_flag = 4'h2;
    else if (room > 128)   push_flag = 4'h3;
    else if (room >= 64)   push_flag = 4'h4;
    else if (room >= 32)   push_flag = 4'hA;
    else if (room >= 16)   push_flag = 4'hB;
    else if (room >= 8)    push_flag = 4'hC;
    else if (room >= 4)    push_flag = 4'hD;
    else if (room >= 2)    push_flag = 4'hE;
    else                   push_flag = 4'hF;
end

assign POP_FLAG     = pop_flag;
assign PUSH_FLAG    = push_flag;
assign Almost_Full  = (count >= 10'd511);
assign Almost_Empty = (count <= 10'd1);
`endif

endmodule
