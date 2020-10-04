with Ada.Text_IO;
use Ada.Text_IO;

with Ada.Real_Time;
use Ada.Real_Time;

with Buffer;
use Buffer;

with Ada.Numerics.Discrete_Random;

procedure ProducerConsumer_Prot is

   	N : constant Integer := 10; -- Number of produced and consumed tokens per task
   	X : constant Integer := 3;  -- Number of producers and consumers

	-- Random Delays
   	subtype Delay_Interval is Integer range 50..250;
   	package Random_Delay is new Ada.Numerics.Discrete_Random (Delay_Interval);
   	use Random_Delay;
   	G : Generator;

   	-- Buffer definition
    CB: CircularBuffer;

   	task type Producer;
   	task type Consumer;
   
   	task body Producer is
    	Next : Time;   -- define next as a time
   	begin
    	Next := Clock; -- assign at the time next the value clock that will be implemented later
		for I in 1..N loop
			
        	-- write on the buffer CB inserting the value I
       	 	CB.Put(I);
         	Put_Line("Produce giving: "&I'Img); 
	 		
         	-- Next 'Release' in 50..250ms
         	Next := Next + Milliseconds(Random(G));
         	delay until Next;
      	end loop;
	end;

   task body Consumer is
      Next : Time;
      R : Integer;
   begin
      Next := Clock;
      for I in 1..N loop

        -- Read from X on the buffer CB
        CB.Get(R); 
		Put_Line("Consume taking: "&R'Img);
		
        Next := Next + Milliseconds(Random(G));
        delay until Next;
      end loop;
   end;
	
	-- Parallelize the work between different producer/costumer
	-- both P and C have to run X time so the number of producer
	P: array (Integer range 1..X) of Producer;
	C: array (Integer range 1..X) of Consumer;
	
begin -- main task
   null;
end ProducerConsumer_Prot;


