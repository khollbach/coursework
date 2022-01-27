module snake(
// clocks
	input CLOCK_50, // 50 MHz
	input CLOCK_27, // 27 MHz
// keys, switches, hex displays, LEDs
	input [3:0] KEY,
	input [17:0] SW,
	output [6:0] HEX0, HEX1, HEX2, HEX3, HEX4, HEX5, HEX6, HEX7,
	output [8:0] LEDG,
	output [17:0] LEDR,
// misc
	inout [35:0] GPIO_0, GPIO_1,
	output TD_RESET, // TV Decoder Reset (THIS IS CRUCIAL; DO NOT REMOVE)
// PS2 (i.e. keyboard input)
	input PS2_DAT, // PS2 data line
	input PS2_CLK, // PS2 clock line
// VGA (i.e. screen output)
	output VGA_CLK, // VGA Clock
	output VGA_HS, // VGA H_SYNC
	output VGA_VS, // VGA V_SYNC
	output VGA_BLANK, // VGA BLANK
	output VGA_SYNC, // VGA SYNC
	output [9:0] VGA_R, // VGA Red[9:0]
	output [9:0] VGA_G, // VGA Green[9:0]
	output [9:0] VGA_B // VGA Blue[9:0]
);

// Reset Delay gives some time for peripherals to initialize.
// (starts low; goes high after 2^20 clock cycles)
wire DLY_RST;
Reset_Delay Reset_Delay1(CLOCK_50, DLY_RST); // NOTE: THIS IS ABSOLUTELY NECESSARY

// KEY[0] is a global reset key
// (active-low; i.e. when pushed).
wire RST;
assign RST = KEY[0] & DLY_RST;

// misc.
assign GPIO_0 = 36'hz_zzzz_zzzz;
assign GPIO_1 = 36'hz_zzzz_zzzz;
assign TD_RESET = 1'b1; // (MUST BE SET HIGH)
assign LEDG[0] = ~KEY[0];
assign LEDG[1] = ~KEY[0];
assign LEDG[2] = ~KEY[1];
assign LEDG[3] = ~KEY[1];
assign LEDG[8] = ~RST;

//////////////////////////
// ps2lab1.v (PS2 code) //
//////////////////////////

wire read, scan_ready;
wire [7:0] scan_code; // code read from keyboard module

reg [7:0] history [3:0]; // history of last four scan_codes

// pulse 'read' on positive edges of 'scan_ready'
oneshot oneshot1(
	.pulse_out(read),
	.trigger_in(scan_ready),
	.clk(CLOCK_50)
);

keyboard keyboard1(
	.keyboard_clk(PS2_CLK),
	.keyboard_data(PS2_DAT),
	.clock50(CLOCK_50),
	.reset(~RST), // (active-high)
	.read(read),
	.scan_ready(scan_ready),
	.scan_code(scan_code)
);

initial
begin
	history[3] <= 8'b0000_0000;
	history[2] <= 8'b0000_0000;
	history[1] <= 8'b0000_0000;
	history[0] <= 8'b0000_0000;
end

always @ (posedge scan_ready) // on scan_ready posedges
begin
	// update history of scan_codes
	history[3] <= history[2];
	history[2] <= history[1];
	history[1] <= history[0];
	history[0] <= scan_code;
end

// display keycodes on hexes
//hex_7seg hex_7seg7(history[3][7:4], HEX7);
//hex_7seg hex_7seg6(history[3][3:0], HEX6);
//
//hex_7seg hex_7seg5(history[2][7:4], HEX5);
//hex_7seg hex_7seg4(history[2][3:0], HEX4);

hex_7seg hex_7seg3(history[1][7:4], HEX3);
hex_7seg hex_7seg2(history[1][3:0], HEX2);

hex_7seg hex_7seg1(history[0][7:4], HEX1);
hex_7seg hex_7seg0(history[0][3:0], HEX0);

//////////////////////////
// vgalab1.v (VGA code) //
//////////////////////////

wire VGA_CTRL_CLK;
wire AUD_CTRL_CLK; // unused, since no audio

wire [9:0] mVGA_R;
wire [9:0] mVGA_G;
wire [9:0] mVGA_B;
wire [9:0] mCoord_X;
wire [9:0] mCoord_Y;

// this module generates various clock outputs, presumably
VGA_Audio_PLL VGA_Audio_PLL1(
	.areset(~RST), // active-high
	.inclk0(CLOCK_27), // clock input
	.c0(VGA_CTRL_CLK), // clock outpus
	.c1(AUD_CTRL_CLK),
	.c2(VGA_CLK)
);

wire [9:0] r, g, b; // assigned values in the game-logic section

wire [9:0] r1, g1, b1;
wire [9:0] r2, g2, b2;

bars bars1(mCoord_X, mCoord_Y, r1, g1, b1);
square square1(mCoord_X, mCoord_Y, r2, g2, b2);

wire [9:0] gray =
mCoord_X < 80 || mCoord_X > 560 ?
10'h000 :
(mCoord_Y / 15) << 5 | (mCoord_X - 80) / 15
;

assign mVGA_R = SW[1] ? (SW[0] ? r : r2) : (SW[0] ? gray : r1);
assign mVGA_G = SW[1] ? (SW[0] ? g : g2) : (SW[0] ? gray : g1);
assign mVGA_B = SW[1] ? (SW[0] ? b : b2) : (SW[0] ? gray : b1);

vga_sync vga_sync1(
	.iCLK(VGA_CTRL_CLK),
	.iRST_N(RST), // active-low
	.iRed(mVGA_R),
	.iGreen(mVGA_G),
	.iBlue(mVGA_B),
// pixel coordinates
	.px(mCoord_X), // (outputs)
	.py(mCoord_Y),
// VGA Side
	.VGA_R(VGA_R),
	.VGA_G(VGA_G),
	.VGA_B(VGA_B),
	.VGA_H_SYNC(VGA_HS),
	.VGA_V_SYNC(VGA_VS),
	.VGA_SYNC(VGA_SYNC),
	.VGA_BLANK(VGA_BLANK)
);

////////////////
// game logic //
////////////////

// game parameters
parameter
// grid dimensions
// these appear in map_color.v as well
x_max = 32'd64,
y_max = 32'd48,
index_max = 32'd3071, // 3071 == (64*48 - 1)
// starting locations
p1_x_start = 32'd16,
p1_y_start = 32'd24,
p2_x_start = 32'd48,
p2_y_start = 32'd24
;

// constants / enumerated vales
parameter
LEFT  = 2'b00,
RIGHT = 2'b01,
UP    = 2'b10,
DOWN  = 2'b11,
// key-codes
// key-code for a special key; next 8-bits is which key
SPECIAL_KEY = 8'he0,
// key-code for a lifted key; next 8-bit code is which key
// (or next 16 bits in the case of a special key)
KEY_LIFT = 8'hf0,
// LRUD input keys
// p1
L1 = 8'h1c,
R1 = 8'h23,
U1 = 8'h1d,
D1 = 8'h1b,
// p2
L2 = 8'h6b,
R2 = 8'h74,
U2 = 8'h75,
D2 = 8'h72
;

// the grid that gets displayed on the screen
// map[x][y] -> 0 if blank square; 1 if filled.
//reg [y_max - 1 : 0] map [x_max - 1 : 0];
reg map [x_max - 1 : 0][y_max - 1 : 0];
//reg [1:0] map [x_max - 1 : 0][y_max - 1 : 0];

assign LEDR[3] = map[p1_x_start + 3][p1_y_start];
assign LEDR[2] = map[p1_x_start + 2][p1_y_start];
assign LEDR[1] = map[p1_x_start + 1][p1_y_start];
assign LEDR[0] = map[p1_x_start + 0][p1_y_start];

// player coordinates and directions
// dir -> current direction
// input_dir -> the dir the player most recently pressed
// p1
reg [9:0] p1_x, p1_y;
reg [1:0] p1_dir, p1_input_dir;
// p2
reg [9:0] p2_x, p2_y;
reg [1:0] p2_dir, p2_input_dir;

assign LEDR[17:8] = p1_x;
assign LEDR[7:4] = {p1_dir,p1_input_dir};

reg [3:0] p1_wins, p2_wins;
hex_7seg hex_7seg7(0, HEX7);
hex_7seg hex_7seg6(p1_wins, HEX6);
hex_7seg hex_7seg5(0, HEX5);
hex_7seg hex_7seg4(p2_wins, HEX4);

// gameover tracker
reg game_over;

// game reset
wire game_reset;
assign game_reset = KEY[1] & DLY_RST & (~game_over);

// game fps clock
wire game_clock;
generate_game_clock generate_game_clock1(CLOCK_50, game_clock);
assign LEDG[7] = game_clock;

// give input to VGA screen
//map_color map_color1(map, mCoord_X, mCoord_Y, r, g, b); // WTF?
wire is_colored;
assign is_colored = map[mCoord_X / 32'd10][mCoord_Y / 32'd10]; // (integer division truncates!)
assign r = is_colored ? 10'h222 : 10'h111;
assign g = is_colored ? 10'h111 : 10'h111;
assign b = is_colored ? 10'h111 : 10'h111;

// get input for player directions after every scan_ready tick
// "negedge read" allows history[3:0] a cycle of CLOCK_50 to update itself
always @ (negedge read, negedge game_reset)
begin
	if (~game_reset)
	begin
		p1_input_dir <= RIGHT;
		p2_input_dir <= LEFT;
	end
	else
	begin
		if (history[1] != KEY_LIFT && history[0] != KEY_LIFT && history[0] != SPECIAL_KEY) // ignore key-lifts
		begin
			// set player directions based on the keypress
			case (history[0])
			// p1
				L1: if (p1_dir != RIGHT) p1_input_dir <= LEFT;
				R1: if (p1_dir != LEFT) p1_input_dir <= RIGHT;
				U1: if (p1_dir != DOWN) p1_input_dir <= UP;
				D1: if (p1_dir != UP) p1_input_dir <= DOWN;
			// p2
				L2: if (p2_dir != RIGHT) p2_input_dir <= LEFT;
				R2: if (p2_dir != LEFT) p2_input_dir <= RIGHT;
				U2: if (p2_dir != DOWN) p2_input_dir <= UP;
				D2: if (p2_dir != UP) p2_input_dir <= DOWN;
			endcase
		end
	end
end

reg [31:0] i, j;

// move players on game_clock ticks
always @ (posedge game_clock, negedge game_reset) // (active-low reset)
begin
	if (~game_reset)
	begin
		//map = 0;
		for (i = 0; i < x_max; i = i + 1)
			for (j = 0; j < y_max; j = j + 1)
				map[i][j] = 1'b0;
		map[p1_x_start][p1_y_start] = 1'b1;
		map[p2_x_start][p2_y_start] = 1'b1;
		
		p1_x = p1_x_start;
		p1_y = p1_y_start;
		p1_dir = RIGHT;
		
		p2_x = p2_x_start;
		p2_y = p2_y_start;
		p2_dir = LEFT;
		
		if (~(KEY[1] & DLY_RST))
		begin
			p1_wins = 0;
			p2_wins = 0;
		end
		
		game_over = 1'b0;
	end
	else
	begin
		// update the directions that
		//   the players are now facing
		p1_dir = p1_input_dir;
		p2_dir = p2_input_dir;
		// move players
		case (p1_dir)
			LEFT: p1_x = p1_x - 1;
			RIGHT: p1_x = p1_x + 1;
			UP: p1_y = p1_y - 1; // up and down are switched on the VGA
			DOWN: p1_y = p1_y + 1;
		endcase
		case (p2_dir)
			LEFT: p2_x = p2_x - 1;
			RIGHT: p2_x = p2_x + 1;
			UP: p2_y = p2_y - 1; // up and down are switched on the VGA
			DOWN: p2_y = p2_y + 1;
		endcase
		
		// detect collisions and players moving off-screen
		if ((p1_x >= x_max || p1_y >= y_max || map[p1_x][p1_y]) &&
			(p2_x >= x_max || p2_y >= y_max || map[p2_x][p2_y]))
		begin
			game_over = 1'b1; // tie
		end
		else if (p1_x >= x_max || p1_y >= y_max || map[p1_x][p1_y])
		begin
			p2_wins = p2_wins + 1'h1;
			game_over = 1'b1;
		end
		else if (p2_x >= x_max || p2_y >= y_max || map[p2_x][p2_y])
		begin
			p1_wins = p1_wins + 1'h1;
			game_over = 1'b1;
		end
		else if (p1_x == p2_x && p1_y == p2_y)
		begin
			game_over = 1'b1; // tie
		end
		else
		begin
			// reflect changes on the gameboard
			map[p1_x][p1_y] = 1'b1;
			map[p2_x][p2_y] = 1'b1;
		end
	end
end

endmodule
