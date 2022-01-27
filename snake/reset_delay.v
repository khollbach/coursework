// oRESET starts low and goes high after (2^20 - 1) clock cycles

module Reset_Delay(iCLK, oRESET);

input iCLK;
output oRESET;

reg [19:0] count;
reg oRESET;

initial
begin
        count <= 20'b0;
        oRESET <= 1'b0;
end

always @ (posedge iCLK)
begin
        if (count != 20'hFFFFF) count <= count + 1'b1;
        else oRESET <= 1'b1;
end

endmodule
