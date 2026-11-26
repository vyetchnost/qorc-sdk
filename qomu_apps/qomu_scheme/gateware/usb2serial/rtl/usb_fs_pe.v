module usb_fs_pe #(
  parameter [4:0] NUM_OUT_EPS = 1,
  parameter [4:0] NUM_IN_EPS = 1
) (
  input clk_48mhz,
  input clk,
  input [6:0] dev_addr,

  ////////////////////////////////////////////////////////////////////////////////
  ////////////////////////////////////////////////////////////////////////////////
  /// USB Endpoint Interface
  ////////////////////////////////////////////////////////////////////////////////
  ////////////////////////////////////////////////////////////////////////////////

  ////////////////////
  // global endpoint interface 
  ////////////////////
  input reset,


  ////////////////////
  // out endpoint interfaces 
  ////////////////////
  input [NUM_OUT_EPS-1:0] out_ep_req,
  output [NUM_OUT_EPS-1:0] out_ep_grant,
  output [NUM_OUT_EPS-1:0] out_ep_data_avail,
  output [NUM_OUT_EPS-1:0] out_ep_setup,
  input [NUM_OUT_EPS-1:0] out_ep_data_get,
  output [7:0] out_ep_data,
  input [NUM_OUT_EPS-1:0] out_ep_stall,
  output [NUM_OUT_EPS-1:0] out_ep_acked,


  ////////////////////
  // in endpoint interfaces 
  ////////////////////
  input [NUM_IN_EPS-1:0] in_ep_req,
  output [NUM_IN_EPS-1:0] in_ep_grant,
  output [NUM_IN_EPS-1:0] in_ep_data_free,
  input [NUM_IN_EPS-1:0] in_ep_data_put,
  input [(NUM_IN_EPS*8)-1:0] in_ep_data,
  input [NUM_IN_EPS-1:0] in_ep_data_done,
  input [NUM_IN_EPS-1:0] in_ep_stall,
  output [NUM_IN_EPS-1:0] in_ep_acked,

  
  ////////////////////
  // sof interface
  ////////////////////
  output sof_valid,
  output [10:0] frame_index,



  ////////////////////////////////////////////////////////////////////////////////
  ////////////////////////////////////////////////////////////////////////////////
  /// USB TX/RX Interface
  ////////////////////////////////////////////////////////////////////////////////
  ////////////////////////////////////////////////////////////////////////////////
  output usb_p_tx,
  output usb_n_tx,

  input  usb_p_rx,
  input  usb_n_rx,

  output usb_tx_en,

  // qomu_scheme debug counters (saturating):
  //   [15:0] line transitions, [31:16] packet starts,
  //   [47:32] valid packets,   [63:48] transmitted packets
  output [63:0] dbg_counters
);
  // in pe interface
  wire [7:0] arb_in_ep_data;

  // rx interface
  wire bit_strobe;
  wire rx_pkt_start;
  wire rx_pkt_end;
  wire [3:0] rx_pid;
  wire [6:0] rx_addr;
  wire [3:0] rx_endp;
  wire [10:0] rx_frame_num;
  wire rx_data_put;
  wire [7:0] rx_data;
  wire rx_pkt_valid;

  // tx mux
  wire in_tx_pkt_start;
  wire [3:0] in_tx_pid;
  wire out_tx_pkt_start;
  wire [3:0] out_tx_pid;
  
  // tx interface
  wire tx_pkt_start;
  wire tx_pkt_end;
  wire [3:0] tx_pid;
  wire tx_data_avail;
  wire tx_data_get;
  wire [7:0] tx_data;

  // sof interface
  assign sof_valid = rx_pkt_end && rx_pkt_valid && rx_pid == 4'b0101;

  // qomu_scheme debug counters (16 bit, wrapping), read as registers 0x14/0x18:
  //   data packets sent (DATA0/1), packets received with a bad PID or CRC,
  //   valid packets received, ACKs received from the host
  wire dbg_line_flip;                   // from usb_fs_rx, unused here now
  reg  [15:0] dbg_datatx = 16'h0, dbg_bad = 16'h0, dbg_valid = 16'h0, dbg_ack = 16'h0;
  always @(posedge clk) begin
      if (tx_pkt_start && tx_pid[1:0] == 2'b11)         dbg_datatx <= dbg_datatx + 1'b1;
      if (rx_pkt_end && !rx_pkt_valid)                  dbg_bad    <= dbg_bad + 1'b1;
      if (rx_pkt_end && rx_pkt_valid)                   dbg_valid  <= dbg_valid + 1'b1;
      if (rx_pkt_end && rx_pkt_valid && rx_pid == 4'b0010) dbg_ack <= dbg_ack + 1'b1;
  end
  assign dbg_counters = {dbg_ack, dbg_valid, dbg_bad, dbg_datatx};
  assign frame_index = rx_frame_num;


  usb_fs_in_arb #(
    .NUM_IN_EPS(NUM_IN_EPS)
  ) usb_fs_in_arb_inst (
    // endpoint interface 
    .in_ep_req(in_ep_req),
    .in_ep_grant(in_ep_grant),
    .in_ep_data(in_ep_data),

    // protocol engine interface 
    .arb_in_ep_data(arb_in_ep_data)
  );

  usb_fs_out_arb #(
    .NUM_OUT_EPS(NUM_OUT_EPS)
  ) usb_fs_out_arb_inst (
    // endpoint interface 
    .out_ep_req(out_ep_req),
    .out_ep_grant(out_ep_grant)
  );

  usb_fs_in_pe #(
    .NUM_IN_EPS(NUM_IN_EPS)
  ) usb_fs_in_pe_inst (
    .clk(clk),
    .reset(reset),
    .reset_ep({NUM_IN_EPS{1'b0}}),
    .dev_addr(dev_addr),

    // endpoint interface 
    .in_ep_data_free(in_ep_data_free),
    .in_ep_data_put(in_ep_data_put),
    .in_ep_data(arb_in_ep_data),
    .in_ep_data_done(in_ep_data_done),
    .in_ep_stall(in_ep_stall),
    .in_ep_acked(in_ep_acked),

    // rx path 
    .rx_pkt_start(rx_pkt_start),
    .rx_pkt_end(rx_pkt_end),
    .rx_pkt_valid(rx_pkt_valid),
    .rx_pid(rx_pid),
    .rx_addr(rx_addr),
    .rx_endp(rx_endp),
    .rx_frame_num(rx_frame_num),

    // tx path 
    .tx_pkt_start(in_tx_pkt_start),
    .tx_pkt_end(tx_pkt_end),
    .tx_pid(in_tx_pid),
    .tx_data_avail(tx_data_avail),
    .tx_data_get(tx_data_get),
    .tx_data(tx_data)
  );

  usb_fs_out_pe #(
    .NUM_OUT_EPS(NUM_OUT_EPS)
  ) usb_fs_out_pe_inst (
    .clk(clk),
    .reset(reset),
    .reset_ep({NUM_OUT_EPS{1'b0}}),
    .dev_addr(dev_addr),

    // endpoint interface 
    .out_ep_data_avail(out_ep_data_avail),
    .out_ep_data_get(out_ep_data_get),
    .out_ep_data(out_ep_data),
    .out_ep_setup(out_ep_setup),
    .out_ep_stall(out_ep_stall),
    .out_ep_acked(out_ep_acked),

    // rx path 
    .rx_pkt_start(rx_pkt_start),
    .rx_pkt_end(rx_pkt_end),
    .rx_pkt_valid(rx_pkt_valid),
    .rx_pid(rx_pid),
    .rx_addr(rx_addr),
    .rx_endp(rx_endp),
    .rx_frame_num(rx_frame_num),
    .rx_data_put(rx_data_put),
    .rx_data(rx_data),

    // tx path 
    .tx_pkt_start(out_tx_pkt_start),
    .tx_pkt_end(tx_pkt_end),
    .tx_pid(out_tx_pid)
  );

  usb_fs_rx usb_fs_rx_inst (
    .clk_48mhz(clk_48mhz),
    .clk(clk),
    .reset(reset),
    .dp(usb_p_rx),
    .dn(usb_n_rx),
    .bit_strobe(bit_strobe),
    .pkt_start(rx_pkt_start),
    .pkt_end(rx_pkt_end),
    .pid(rx_pid),
    .addr(rx_addr),
    .endp(rx_endp),
    .frame_num(rx_frame_num),
    .rx_data_put(rx_data_put),
    .rx_data(rx_data),
    .valid_packet(rx_pkt_valid),
    .dbg_line_flip(dbg_line_flip)
  );

  usb_fs_tx_mux usb_fs_tx_mux_inst (
    // interface to IN Protocol Engine
    .in_tx_pkt_start(in_tx_pkt_start),
    .in_tx_pid(in_tx_pid),

    // interface to OUT Protocol Engine
    .out_tx_pkt_start(out_tx_pkt_start),
    .out_tx_pid(out_tx_pid),

    // interface to tx module
    .tx_pkt_start(tx_pkt_start),
    .tx_pid(tx_pid)
  );

  // Qomu port: transmitter runs on the 12 MHz clock, one bit per clock
  // (see USB_TX_12MHZ in usb_fs_tx.v).
  usb_fs_tx usb_fs_tx_inst (
    .clk_48mhz(clk),
    .clk(clk),
    .reset(reset),
    .bit_strobe(1'b1),
    .oe(usb_tx_en),
    .dp(usb_p_tx),
    .dn(usb_n_tx),
    .pkt_start(tx_pkt_start),
    .pkt_end(tx_pkt_end),
    .pid(tx_pid),
    .tx_data_avail(tx_data_avail),
    .tx_data_get(tx_data_get),
    .tx_data(tx_data)
  );
endmodule
