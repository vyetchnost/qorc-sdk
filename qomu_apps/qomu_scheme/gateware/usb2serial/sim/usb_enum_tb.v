`timescale 1ps / 1ps


`ifdef continue_on_fail
 `define finish_and_return(code) #0
`else
 `ifdef __ICARUS__
  `define finish_and_return(code) $finish_and_return(code)
 `else
  `define finish_and_return(code) $finish
 `endif
`endif

`define assert(msg, signal, value) \
        if ((signal) !== value) begin \
            $display("%d ERROR (%m): %s. signal != value", $time, msg); \
            $display("    actual:   %x", signal); \
            $display("    expected: %x", value); \
        end

`define assert_true(msg, signal) \
        if (!(signal)) begin \
            $display("%d ERROR (%m): %s. (signal) == FALSE", $time, msg); \
        end



// qomu_scheme: USB enumeration testbench for the open-toolchain port.
// The USB host model tasks are taken from s3-gateware/usb2serial/rtlsim/
// enum_test_top.v (TinyFPGA bootloader testbench); the harness is new:
// our 3-pin top level, ASSP clocks forced, toolchain RAM/FIFO models.
`timescale 1ps/1ps
module top_tb;

    reg clk_48mhz = 0;
    reg clk_24 = 0;
    reg clk_12mhz = 0;
    reg reset = 1;

    always #10416 clk_48mhz = !clk_48mhz;          // 48 MHz
    always @(posedge clk_48mhz) clk_24 <= !clk_24;
    always @(posedge clk_24) clk_12mhz <= !clk_12mhz; // 12 MHz, in phase

    initial begin
        #200000 reset = 0;
    end

    // USB bus seen from the host side
    reg usb_p_rx = 1'b1;
    reg usb_n_rx = 1'b0;
    wire usbp_io, usbn_io, usb_pu;
    wire usb_tx_en = dut.u_AL4S3B_FPGA_IP.usb_tx_en;
    wire usb_p_tx = usb_tx_en ? usbp_io : 1'b1;
    wire usb_n_tx = usb_tx_en ? usbn_io : 1'b0;
    assign usbp_io = (!usb_tx_en) ? usb_p_rx : 1'bz;
    assign usbn_io = (!usb_tx_en) ? usb_n_rx : 1'bz;

    AL4S3B_FPGA_top dut (
        .usbp_io        ( usbp_io ),
        .usbn_io        ( usbn_io ),
        .usb_pu_cntrl_o ( usb_pu  )
    );

    // The ASSP (qlal4s3b_cell_macro) model has no behaviour: drive its
    // clock / reset outputs as the chip would.
    initial begin
        force dut.Sys_Clk1     = clk_48mhz;
        force dut.Sys_Clk0     = clk_12mhz;
        force dut.Sys_Clk1_Rst = reset;
        force dut.Sys_Clk0_Rst = reset;
        force dut.WB_RST       = reset;
        force dut.WBs_CYC      = 1'b0;
        force dut.WBs_STB      = 1'b0;
        force dut.WBs_WE       = 1'b0;
        force dut.WBs_RD       = 1'b0;
        force dut.WBs_ADR      = 17'h0;
        force dut.WBs_WR_DAT   = 32'h0;
        force dut.WBs_BYTE_STB = 4'h0;
    end


    integer errors = 0;

    // ---- Wishbone master standing in for the M4 (registers at byte offsets) ----
    // Timing as on the chip: request at a falling edge, the slave registers ACK
    // on the next rising edge, the transfer completes on the rising edge after.
    task wb_write;
      input [16:0] addr;
      input [31:0] data;
    begin
      @(negedge clk_12mhz);
      force dut.WBs_ADR = addr; force dut.WBs_WR_DAT = data; force dut.WBs_BYTE_STB = 4'hF;
      force dut.WBs_WE = 1'b1;  force dut.WBs_CYC = 1'b1;    force dut.WBs_STB = 1'b1;
      @(posedge clk_12mhz);
      @(negedge clk_12mhz);
      `assert_true("wishbone write acknowledged", dut.WBs_ACK)
      @(posedge clk_12mhz);
      @(negedge clk_12mhz);
      force dut.WBs_WE = 1'b0;  force dut.WBs_CYC = 1'b0;    force dut.WBs_STB = 1'b0;
      force dut.WBs_BYTE_STB = 4'h0;
    end
    endtask

    task wb_read;
      input  [16:0] addr;
      output [31:0] data;
    begin
      @(negedge clk_12mhz);
      force dut.WBs_ADR = addr; force dut.WBs_BYTE_STB = 4'hF;
      force dut.WBs_RD = 1'b1;  force dut.WBs_CYC = 1'b1;    force dut.WBs_STB = 1'b1;
      @(posedge clk_12mhz);
      @(negedge clk_12mhz);
      `assert_true("wishbone read acknowledged", dut.WBs_ACK)
      data = dut.WBs_RD_DAT;
      @(posedge clk_12mhz);
      @(negedge clk_12mhz);
      force dut.WBs_RD = 1'b0;  force dut.WBs_CYC = 1'b0;    force dut.WBs_STB = 1'b0;
      force dut.WBs_BYTE_STB = 4'h0;
    end
    endtask

    task wb_expect;
      input [16:0]  addr;
      input [31:0]  value;
      input [8*24:1] what;
      reg   [31:0]  got;
    begin
      wb_read(addr, got);
      if (got !== value) begin
        $display("%d ERROR (wb_expect): %0s: read %08x from %03x, expected %08x", $time, what, got, addr, value);
        errors = errors + 1;
      end else
        $display("   ok: %0s = %0x", what, got);
    end
    endtask

    task wait_usb_interpacket_delay;
    begin
      #83328;
      #83328;
      #83328;
      #83328;
      #83328;
      #83328;
    end
    endtask

    task wait_usb_eop;
    begin
      while (!usb_tx_en) #83328;
      while (usb_tx_en) #83328;
      wait_usb_interpacket_delay();
    end
    endtask

    task send_usb_raw_bit;
      input usbp;
      input usbn;
    begin
        usb_p_rx <= usbp;
        usb_n_rx <= usbn;
        #83328;
    end
    endtask

    task send_usb_k;
    begin
      send_usb_raw_bit(1'b0, 1'b1);
    end
    endtask

    task send_usb_j;
    begin
      send_usb_raw_bit(1'b1, 1'b0); 
    end
    endtask

    task send_usb_se0;
    begin
      send_usb_raw_bit(1'b0, 1'b0); 
    end
    endtask
    
    task send_usb_port_reset;
    begin
      send_usb_se0();
     // #1250000000; 
	  #125000000; 
      reset = 1;
      send_usb_j();
      reset = 0;
    end
    endtask

    integer i;
    reg send_usb_nrzi_state;
    integer send_usb_bitstuff_count;
    task send_usb_raw;
      input [1023:0] payload;
      input [10:0] length;
    begin
      // sync
      send_usb_k();
      send_usb_j();
      send_usb_k();
      send_usb_j();
      send_usb_k();
      send_usb_j();
      send_usb_k();
      send_usb_k();

      // payload
      send_usb_nrzi_state = 0;
      send_usb_bitstuff_count = 1;
      for (i = 0; i < length; i = i + 1) begin
        // keep track of nrzi state
        // (toggle line state for '0', keep steady for '1')
        if (!payload[i]) begin
          send_usb_nrzi_state = !send_usb_nrzi_state;
        end

        // send the data bit
        send_usb_raw_bit(send_usb_nrzi_state, !send_usb_nrzi_state);
        
        // keep track of bitstuff
        // (how many '1's in a row)
        if (payload[i]) begin
          send_usb_bitstuff_count = send_usb_bitstuff_count + 1;
        end else begin
          send_usb_bitstuff_count = 0;
        end

        // insert a '0' data bit if we hit bitstuff condition
        if (send_usb_bitstuff_count >= 6) begin
          send_usb_bitstuff_count = 0;
          send_usb_nrzi_state = !send_usb_nrzi_state;
          send_usb_raw_bit(send_usb_nrzi_state, !send_usb_nrzi_state);
        end
      end

      // eop
      send_usb_se0();
      send_usb_se0();
      send_usb_j();
      wait_usb_interpacket_delay();
    end
    endtask

    task send_usb_handshake;
      input [3:0] pid;
    begin
      send_usb_raw({~pid, pid}, 8);      
    end
    endtask

    task send_usb_ack;
    begin
      send_usb_handshake(4'b0010);
    end
    endtask

    task send_usb_nak;
    begin
      send_usb_handshake(4'b1010);
    end
    endtask

    task send_usb_stall;
    begin
      send_usb_handshake(4'b1110);
    end
    endtask

    task send_usb_nyet;
    begin
      send_usb_handshake(4'b0110);
    end
    endtask

    reg [4:0] crc5;
    reg crc5_invert; 
    task calc_crc5;
      input [10:0] data;
    begin
      crc5 = 5'b11111;

      for (i = 0; i < 11; i = i + 1) begin
        crc5_invert = data[i] ^ crc5[4]; 

        crc5[4] = crc5[3];
        crc5[3] = crc5[2];
        crc5[2] = crc5[1] ^ crc5_invert;
        crc5[1] = crc5[0];
        crc5[0] = crc5_invert;
      end
    end
    endtask

    task send_usb_token;
      input [3:0] pid;
      input [10:0] data;
    begin
      calc_crc5(data);
      send_usb_raw({~crc5[0], ~crc5[1], ~crc5[2], ~crc5[3], ~crc5[4], data, ~pid, pid}, 24);      
    end
    endtask

    task send_usb_in;
      input [6:0] addr;
      input [3:0] endp;
    begin
      send_usb_token(4'b1001, {endp, addr});
    end
    endtask

    task send_usb_out;
      input [6:0] addr;
      input [3:0] endp;
    begin
      send_usb_token(4'b0001, {endp, addr});
    end
    endtask

    task send_usb_setup;
      input [6:0] addr;
      input [3:0] endp;
    begin
      send_usb_token(4'b1101, {endp, addr});
    end
    endtask

    task send_usb_sof;
      input [10:0] frame;
    begin
      send_usb_token(4'b0101, frame);
    end
    endtask


    

    reg [15:0] crc16;
    reg crc16_invert; 
    task calc_crc16;
      input [511:0] data;
      input [10:0] length;
    begin
      crc16 = 16'b1111111111111111;

      for (i = 0; i < length; i = i + 1) begin
        crc16_invert = data[i] ^ crc16[15]; 

        crc16[15] = crc16[14] ^ crc16_invert;
        crc16[14] = crc16[13];
        crc16[13] = crc16[12];
        crc16[12] = crc16[11];
        crc16[11] = crc16[10];
        crc16[10] = crc16[9];
        crc16[9] = crc16[8];
        crc16[8] = crc16[7];
        crc16[7] = crc16[6];
        crc16[6] = crc16[5];
        crc16[5] = crc16[4];
        crc16[4] = crc16[3];
        crc16[3] = crc16[2];
        crc16[2] = crc16[1] ^ crc16_invert;
        crc16[1] = crc16[0];
        crc16[0] = crc16_invert;
      end
    end
    endtask

    reg [1023:0] raw_usb_data;
    task send_usb_data;
      input [3:0] pid;
      input [511:0] data;
      input [10:0] length;
    begin
      calc_crc16(data, length);
      raw_usb_data[3:0] = pid;
      raw_usb_data[7:4] = ~pid;

      for (i = 0; i < length; i = i + 1) begin
        raw_usb_data[8 + i] = data[i];
      end

      raw_usb_data[8 + length +  0] =  ~crc16[15];
      raw_usb_data[8 + length +  1] =  ~crc16[14];
      raw_usb_data[8 + length +  2] =  ~crc16[13];
      raw_usb_data[8 + length +  3] =  ~crc16[12];
      raw_usb_data[8 + length +  4] =  ~crc16[11];
      raw_usb_data[8 + length +  5] =  ~crc16[10];
      raw_usb_data[8 + length +  6] =  ~crc16[9];
      raw_usb_data[8 + length +  7] =  ~crc16[8];
      raw_usb_data[8 + length +  8] =  ~crc16[7];
      raw_usb_data[8 + length +  9] =  ~crc16[6];
      raw_usb_data[8 + length + 10] =  ~crc16[5];
      raw_usb_data[8 + length + 11] =  ~crc16[4];
      raw_usb_data[8 + length + 12] =  ~crc16[3];
      raw_usb_data[8 + length + 13] =  ~crc16[2];
      raw_usb_data[8 + length + 14] =  ~crc16[1];
      raw_usb_data[8 + length + 15] =  ~crc16[0];

      send_usb_raw(raw_usb_data, 8 + length + 16);      
    end
    endtask

    task send_usb_data0;
      input [511:0] data;
      input [10:0] length;
    begin
      send_usb_data(4'b0011, data, length);
    end
    endtask

    task send_usb_data1;
      input [511:0] data;
      input [10:0] length;
    begin
      send_usb_data(4'b1011, data, length);
    end
    endtask

    task send_usb_data2;
      input [511:0] data;
      input [10:0] length;
    begin
      send_usb_data(4'b0111, data, length);
    end
    endtask

    task send_usb_mdata;
      input [511:0] data;
      input [10:0] length;
    begin
      send_usb_data(4'b1111, data, length);
    end
    endtask


    
    reg [7:0] sync;
    reg [1:0] eop;
    reg prev_usb_p_tx;
    integer get_usb_bitstuff_count;
    task get_usb_raw;
      output [1023:0] data;
      output [10:0] length;
    begin
      length = 0;
      sync = 0;
      eop = 2'b10;

      // wait for sync
      while (sync != 8'b01010100) begin
        sync = {sync[6:0], usb_p_tx};
        prev_usb_p_tx = usb_p_tx; 
        #83328;
      end

      get_usb_bitstuff_count = 0;

      // get data
      while (eop != 2'b00) begin
        eop = {usb_p_tx, usb_n_tx};

        if (eop == 2'b00) begin
          // do not save se0 data

        end else if (get_usb_bitstuff_count == 6) begin
          // expect bitstuff
          `assert_true("one '0' must follow six '1's", usb_p_tx != prev_usb_p_tx)
          get_usb_bitstuff_count = 0;
        end else begin
          // regular data bit
          length = length + 1;
          if (usb_p_tx == prev_usb_p_tx) begin
            data = {1'b1, data[1023:1]};
            get_usb_bitstuff_count = get_usb_bitstuff_count + 1;
          end else begin
            data = {1'b0, data[1023:1]};
            get_usb_bitstuff_count = 0;
          end
        end

        prev_usb_p_tx = usb_p_tx;

        #83328;
      end

      // se0 initiates end of packet, must be two bit-lengths according to
      // the USB2 spec
      `assert_true("eop must have two se0 bits", usb_p_tx == 1'b0 && usb_n_tx == 1'b0)

      // shift the data down to the bottom so the first bit is data[0]
      data = data >> (1024 - length);

      wait_usb_interpacket_delay();
    end
    endtask

    reg [1023:0] usb_tx_data;
    reg [10:0] usb_tx_len;

    task expect_usb_handshake;
      input [3:0] pid;
    begin
      get_usb_raw(usb_tx_data, usb_tx_len);
      `assert("handshake packets are 8 bits long", usb_tx_len, 8)
      `assert("handshake pid mismatch", usb_tx_data[3:0], pid)
      `assert("pid complement mismatch", usb_tx_data[7:4], ~usb_tx_data[3:0])
    end
    endtask

    task expect_usb_ack;
    begin
      expect_usb_handshake(4'b0010);
    end
    endtask

    task expect_usb_nak;
    begin
      expect_usb_handshake(4'b1010);
    end
    endtask

    task expect_usb_stall;
    begin
      expect_usb_handshake(4'b1110);
    end
    endtask

    task expect_usb_data;
      input [3:0] pid;
      input [1023:0] data;
      input [10:0] length;
    begin
      calc_crc16(data, length);
      raw_usb_data[3:0] = pid;
      raw_usb_data[7:4] = ~pid;

      for (i = 0; i < length; i = i + 1) begin
        raw_usb_data[8 + i] = data[i];
      end

      raw_usb_data[8 + length +  0] =  ~crc16[15];
      raw_usb_data[8 + length +  1] =  ~crc16[14];
      raw_usb_data[8 + length +  2] =  ~crc16[13];
      raw_usb_data[8 + length +  3] =  ~crc16[12];
      raw_usb_data[8 + length +  4] =  ~crc16[11];
      raw_usb_data[8 + length +  5] =  ~crc16[10];
      raw_usb_data[8 + length +  6] =  ~crc16[9];
      raw_usb_data[8 + length +  7] =  ~crc16[8];
      raw_usb_data[8 + length +  8] =  ~crc16[7];
      raw_usb_data[8 + length +  9] =  ~crc16[6];
      raw_usb_data[8 + length + 10] =  ~crc16[5];
      raw_usb_data[8 + length + 11] =  ~crc16[4];
      raw_usb_data[8 + length + 12] =  ~crc16[3];
      raw_usb_data[8 + length + 13] =  ~crc16[2];
      raw_usb_data[8 + length + 14] =  ~crc16[1];
      raw_usb_data[8 + length + 15] =  ~crc16[0];

      for (i = 8 + length + 16; i < 1024; i = i + 1) begin
        raw_usb_data[i] = 1'b0;
      end

      get_usb_raw(usb_tx_data, usb_tx_len);

      `assert_true("data packets are less than 536 bits long", usb_tx_len < 536)
      `assert_true("data packets are at least 24 bits long", usb_tx_len >= 24)
      `assert("data length", usb_tx_len, length + 24)
      `assert("data mismatch", usb_tx_data, raw_usb_data)
      `assert("data packet lengths are a multiple of 8 bits", usb_tx_len[2:0], 3'b000)
      `assert("pid complement mismatch", usb_tx_data[7:4], ~usb_tx_data[3:0])
      `assert("data pid mismatch", usb_tx_data[3:0], pid)
    end
    endtask
      
    task expect_usb_data1;
      input [1023:0] data;
      input [10:0] length;
    begin
      expect_usb_data(4'b1011, data, length);
    end
    endtask
      
    task expect_usb_data0;
      input [1023:0] data;
      input [10:0] length;
    begin
      expect_usb_data(4'b0011, data, length);
    end
    endtask

    task send_usb_ctrl_xfer;
      input [7:0] addr;
      input [63:0] setup_data;
    begin
      // setup stage
      send_usb_setup(addr, 0); 
      send_usb_data0(setup_data, 64);
      expect_usb_ack();

      // status stage
      send_usb_in(addr, 0);
      expect_usb_data1(0, 0);
      send_usb_ack();
    end
    endtask


    task send_usb_address_device;
      input [7:0] old_addr;
      input [7:0] new_addr;
    begin
      send_usb_ctrl_xfer(old_addr, {8'h00, 8'h00, 8'h00, 8'h00, 8'h00, new_addr, 8'h05, 8'h00});
      //`assert("new device address", dut.dev_addr, 7'h1e)
      `assert("new device address", dut.u_AL4S3B_FPGA_IP.u_usb2m4_serial.ctrl_ep_inst.dev_addr, 7'h1e)
    end
    endtask
	

    initial begin
        if ($test$plusargs("vcd")) begin
            $dumpfile("usb_enum_tb.vcd");
            // depth 1 per block keeps the file small
            $dumpvars(1, top_tb);
            $dumpvars(1, top_tb.dut.u_AL4S3B_FPGA_IP.u_usb2m4_serial);
            $dumpvars(1, top_tb.dut.u_AL4S3B_FPGA_IP.u_usb2m4_serial.ctrl_ep_inst);
            $dumpvars(1, top_tb.dut.u_AL4S3B_FPGA_IP.u_usb2m4_serial.usb_fs_pe_inst);
            $dumpvars(1, top_tb.dut.u_AL4S3B_FPGA_IP.u_usb2m4_serial.usb_fs_pe_inst.usb_fs_rx_inst);
            $dumpvars(1, top_tb.dut.u_AL4S3B_FPGA_IP.u_usb2m4_serial.usb_fs_pe_inst.usb_fs_out_pe_inst);
            $dumpvars(1, top_tb.dut.u_AL4S3B_FPGA_IP.u_usb2m4_serial.usb_fs_pe_inst.usb_fs_in_pe_inst);
        end
        #1000000;
        send_usb_port_reset();
        #1000000;

        $display("== SETUP GET_DESCRIPTOR(device), expect ACK");
        send_usb_setup(0, 0);
        send_usb_data0({8'h00, 8'h40, 8'h00, 8'h00, 8'h01, 8'h00, 8'h06, 8'h80}, 64);
        expect_usb_ack();

        $display("== IN, expect device descriptor");
        send_usb_in(0, 0);
        expect_usb_data1(
          {8'h01, 8'h00, 8'h00, 8'h00, 8'h00, 8'h00, 8'h61, 8'h40, 8'h1d,
           8'h50, 8'h20, 8'h00, 8'h00, 8'h02, 8'h02, 8'h00, 8'h01, 8'h12}, 18 * 8);
        send_usb_ack();

        $display("== status stage");
        send_usb_out(0, 0);
        send_usb_data1(0, 0);
        expect_usb_ack();

        $display("== SET_ADDRESS 0x1e");
        send_usb_address_device(8'h00, 8'h1e);

        // ---- M4 -> USB: two bytes through the m2u FIFO, out as an EP1 IN packet
        $display("== M4 writes 'Hi' to the m2u FIFO");
        wb_expect(17'h080, 32'h1, "m2u flags empty");
        wb_write(17'h084, 32'h48);
        wb_write(17'h084, 32'h69);
        #20000000;                       // let the bridge move them to EP1
        // The bridge arms EP1 with an empty packet whenever the m2u FIFO is
        // empty (upstream behaviour), so the first IN after reset returns a
        // zero-length DATA0; the host polls again and gets the data as DATA1.
        $display("== IN EP1, expect zero-length DATA0 (armed at reset)");
        send_usb_in(8'h1e, 1);
        expect_usb_data0(0, 0);
        send_usb_ack();
        #20000000;
        $display("== IN EP1, expect DATA1 'Hi'");
        send_usb_in(8'h1e, 1);
        expect_usb_data1({8'h69, 8'h48}, 16);
        send_usb_ack();
        wb_expect(17'h080, 32'h1, "m2u flags empty again");

        // ---- USB -> M4: an EP1 OUT packet into the u2m FIFO, read by the M4
        $display("== OUT EP1 'ab', expect ACK");
        wb_expect(17'h040, 32'h0, "u2m flags empty");
        send_usb_out(8'h1e, 1);
        send_usb_data0({8'h62, 8'h61}, 16);
        expect_usb_ack();
        #20000000;
        `assert_true("rx interrupt is low while disabled", !dut.u_AL4S3B_FPGA_IP.Interrupt_o)
        wb_write(17'h0C0, 32'h1);        // u2m_fifo_int_en
        @(negedge clk_12mhz);
        `assert_true("rx interrupt asserted with data waiting", dut.u_AL4S3B_FPGA_IP.Interrupt_o)
        wb_expect(17'h040, 32'h2, "u2m flags >=2 entries");
        wb_expect(17'h044, 32'h61, "u2m rdata 'a'");
        wb_expect(17'h040, 32'h1, "u2m flags 1 entry");
        wb_expect(17'h044, 32'h62, "u2m rdata 'b'");
        wb_expect(17'h040, 32'h0, "u2m flags empty");
        @(negedge clk_12mhz);
        `assert_true("rx interrupt drops when drained", !dut.u_AL4S3B_FPGA_IP.Interrupt_o)

        $display("== debug counters: line transitions %0d, packet starts %0d, valid packets %0d, tx packets %0d",
                 dut.u_AL4S3B_FPGA_IP.usb_dbg_counters[15:0],  dut.u_AL4S3B_FPGA_IP.usb_dbg_counters[31:16],
                 dut.u_AL4S3B_FPGA_IP.usb_dbg_counters[47:32], dut.u_AL4S3B_FPGA_IP.usb_dbg_counters[63:48]);
        $display("== done at %0t", $time);
        $finish;
    end

    initial begin
        #400000000;
        $display("== TIMEOUT");
        $finish;
    end
endmodule
