module generate_game_clock(CLOCK_50, game_clock);

input CLOCK_50;
output game_clock;

reg [21:0] count; //

assign game_clock = count[21]; //

initial count <= 22'h0000_0000; //

always @ (posedge CLOCK_50) count <= count + 1'b1;

endmodule
