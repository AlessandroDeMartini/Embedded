with Ada.Text_IO;
use Ada.Text_IO;

with Ada.Real_Time;
use Ada.Real_Time;

with Ada.Numerics.Discrete_Random;

procedure ProducerConsumer_Rndzvs is
	
   N : constant Integer := 10; -- Number of produced and consumed tokens per task
	X : constant Integer := 3; -- Number of producers and consumers	
	
   -- Random Delays
   subtype Delay_Interval is Integer range 50..250;
   package Random_Delay is new Ada.Numerics.Discrete_Random (Delay_Interval);
   use Random_Delay;
   G : Generator;

   task Buffer is
      entry Append(I : in Integer);
      entry Take(I : out Integer);
   end Buffer;

   task type Producer;

   task type Consumer;
   
   task body Buffer is
         Size: constant Integer := 4;
         type Index is mod Size;
         type Item_Array is array(Index) of Integer;
         B : Item_Array;
         In_Ptr, Out_Ptr, Count : Index := 0;
   begin
      loop
         select
		accept Append(I : in Integer) when Count < Size do		-- => Complete Code: Service Append
			B(In_Ptr) := X;
         		In_Ptr    := In_Ptr + 1;
         		Count     := Count + 1;
		end Append;
         or
		accept Take(I : out Integer) when Count > 0 do		-- => Complete Code: Service Take
			X := B(Out_Ptr);
        		Out_Ptr := Out_Ptr + 1;
         		Count := Count - 1;
		end Take;
         or
		terminate;		-- => Termination
         end select;
      end loop;
   end Buffer;
      
   task body Producer is
      Next : Time;
   begin
      Next := Clock;
      for I in 1..N loop
			
        B.Append(I);  -- => Complete code: Write to X
	Put_Line("Producer giving: " &I'Img);

         -- Next 'Release' in 50..250ms
         Next := Next + Milliseconds(Random(G));
         delay until Next;
      end loop;
   end;

   task body Consumer is
      Next : Time;
      X : Integer;
   begin
      Next := Clock;
      for I in 1..N loop
        B.Take(X);  -- Complete Code: Read from X
	Put_Line("Consumer giving: " &X'Img);

         Next := Next + Milliseconds(Random(G));
         delay until Next;
      end loop;
   end;
	
	P: array (Integer range 1..X) of Producer;
	C: array (Integer range 1..X) of Consumer;
	
begin -- main task
   null;
end ProducerConsumer_Rndzvs;


