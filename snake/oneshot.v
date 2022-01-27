// Pulses the output 'pulse_out' whenever
// the input 'trigger_in' goes from low to high.

module oneshot(pulse_out, trigger_in, clk);

output pulse_out;
input trigger_in;
input clk;

reg prev_in;
reg pulse_out;

initial
begin
	prev_in <= 1'b1;
	pulse_out <= 1'b0;
end

always @ (posedge clk)
begin
	// when the input goes low to high, pulse (i.e. set output high)
	if (!prev_in && trigger_in) pulse_out <= 1'b1;
	// (if the input is low, or if it was already high, output is low)
	else pulse_out <= 1'b0;
	prev_in <= trigger_in;
end

endmodule
