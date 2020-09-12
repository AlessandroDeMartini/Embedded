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

   -- Buffer Definition
	Size: constant Integer := 3;
   subtype Item is Integer;
	type Index is mod Size;
	type Item_Array is array(Index) of Item;

	A : Item_Array;
	In_Ptr, Out_Ptr, Count : Index := 0;

   task type Producer;
   task type Consumer;

   task body Producer is
      Next : Time;
   begin
      Next := Clock;

      for I in 1..N loop
			
         -- ==> Complete code: Write to Buffer
			A := Buffer.Put(I);
         Put_Line(Integer'Image(I));

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
         -- Read from X

         -- ==> Complete code: Read from Buffer
			-- := Buffer.Get(X);

         Put_Line(Integer'Image(X));
         Next := Next + Milliseconds(Random(G));
         delay until Next;
      end loop;
   end;
	
	P: array (Integer range 1..X) of Producer;
	C: array (Integer range 1..X) of Consumer;
	
begin -- main task
   null;
end ProducerConsumer_Prot;


