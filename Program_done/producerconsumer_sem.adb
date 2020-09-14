with Ada.Text_IO;
use Ada.Text_IO;

with Ada.Real_Time;
use Ada.Real_Time;

with Ada.Numerics.Discrete_Random;

with Semaphores;
use Semaphores;

procedure ProducerConsumer_Sem is
	
	N : constant Integer := 10;                     -- Number of produced and consumed tokens per task
	X : constant Integer := 3;                      -- Number of producers and consumer

	-- Buffer Definition
	Size: constant Integer := 3;                    -- Buffer size
	type Index is mod Size;                         -- index indicate a modular type
	type Item_Array is array(Index) of Integer;     -- item array indicate a type like the buffer one
	B : Item_Array;                                 -- Buffer definition
	In_Ptr, Out_Ptr, Count : Index := 0;            -- In_Ptr  -> position where write, 
													-- Out_Ptr -> position where read, 
													-- Count   -> elements in the buffer

	-- Random Delays
	subtype Delay_Interval is Integer range 50..250;
	package Random_Delay is new Ada.Numerics.Discrete_Random (Delay_Interval);
	use Random_Delay;
	G : Generator;
	
	
	-- Declation of Semaphores
	--    1. Semaphore 'NotFull' to indicate that buffer is not full
	--    2. Semaphore 'NotEmpty' to indicate that buffer is not empty
	--    3. Semaphore 'AtomicAccess' to ensure an atomic access to the buffer
	
	NotFull: CountingSemaphore(3,3);      -- Max = 3, Init = 3 -> Start Notfull with 3 spots
	NotEmpty: CountingSemaphore(3,0);     -- Max = 3, Init = 0 -> Start Empty, there can be three elements
	AtomicAccess: CountingSemaphore(1,1); -- Max = 1, Init = 1 -> Bulean Buffer (one access at a time), start free
	
	task type Producer;
	task type Consumer;

	task body Producer is
		Next : Time;
	begin
	Next := Clock;
		for I in 1..N loop
      		
			NotFull.Wait;             -- I can access the buffer if it is not full
			AtomicAccess.Wait;        -- I can do just one operation at a time
			
			-- Write in the buffer
			B(In_Ptr) := I;           -- I write Buffer position In_Ptr the Operation number I
         	In_Ptr    := In_Ptr + 1;  -- Incrementation, feel the next buffer space
			
			AtomicAccess.Signal;      -- Once I wrote in the buffer I can do another operation		
			NotEmpty.Signal;          -- Since I'm in the produces I fell empty spaces

			Put_Line("Produce giving: "&I'Img); 
      		     		
        	-- Next 'Release' in 50..250ms
        	Next := Next + Milliseconds(Random(G));
        	
   			delay until Next;
         
		end loop;
	end;

	task body Consumer is
	    Next : Time;
		R    : Integer;  -- Variable used to read from the buffer
	begin
	Next := Clock;
    	for I in 1..N loop
 
			NotEmpty.Wait;          -- I can read from the buffer if it is not Empty
			AtomicAccess.Wait;      -- Since I'm doing an operation I have to ask permition
			
			-- Read from the buffer
			R       := B(Out_Ptr);  -- Read from position Out_Ptr
         	Out_Ptr := Out_Ptr + 1; -- Out_Ptr incrementation, I need to watch the next element (Out_ptr is a modular)

			AtomicAccess.Signal;    -- Once I do the operation I done, another can acess the buffer
			NotFull.Signal;         -- If I read from the buffer I let free one buffer spot 

			Put_Line("Produce taking: "&R'Img); 

			-- Next 'Release' in 50..250ms
         	Next := Next + Milliseconds(Random(G));


        delay until Next;
		end loop;
	end;
	
	P: array (Integer range 1..X) of Producer;
	C: array (Integer range 1..X) of Consumer;
	
begin -- main task
   null;
end ProducerConsumer_Sem;


