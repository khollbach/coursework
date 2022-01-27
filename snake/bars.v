// maps (x, y) to colors;
// by inspection, the screen is colored with solid vertical bars.

module bars(x, y, r, g, b);

input [9:0] x, y;
output [9:0] r, g, b;

reg [2:0] idx;

assign r = idx[0] ? 10'h3ff : 10'h000;
assign g = idx[1] ? 10'h3ff : 10'h000;
assign b = idx[2] ? 10'h3ff : 10'h000;

initial idx <= 3'd0;

always @ (x) begin
	if (x < 80) idx <= 3'd0; // black
	else if (x < 160) idx <= 3'd1; // red
	else if (x < 240) idx <= 3'd2; // green
	else if (x < 320) idx <= 3'd3; // r+g = yellow
	else if (x < 400) idx <= 3'd4; // blue
	else if (x < 480) idx <= 3'd5; // r+b = magenta
	else if (x < 560) idx <= 3'd6; // g+b = cyan
	else idx <= 3'd7; // white
end

endmodule
