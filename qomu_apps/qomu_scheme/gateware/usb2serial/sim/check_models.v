// Stand-in models used only by `build.sh check` (iverilog). ql_symbiflow maps
// these cells itself during synthesis, but pp3_cells_sim.v has no model.
module gclkbuff (input A, output Z);
  assign Z = A;
endmodule
