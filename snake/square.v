// maps (x, y) to colors; namely, a square
// The display is 640x480 pixels (4:3)

module square(x, y, r, g, b);

input [9:0] x, y;
output [9:0] r, g, b;

wire chi_s; // characterizes the square

assign chi_s = (200 <= x && x < 440 && 120 <= y && y < 360);

assign r = 10'h000;
assign g = chi_s ? 10'h3ff : 10'h000;
assign b = chi_s ? 10'h3ff : 10'h000;

endmodule
