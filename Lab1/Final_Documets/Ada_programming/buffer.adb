package body Buffer is
   
	protected body CircularBuffer is

      	entry Put(X: in Item) when Count < Size is
      	begin
         	A(In_Ptr) := X;
         	In_Ptr    := In_Ptr + 1;
         	Count     := Count + 1;
      	end Put;

      	entry Get(X: out Item) when Count > 0 is
      	begin
       		X       := A(Out_Ptr);
         	Out_Ptr := Out_Ptr + 1;
         	Count   := Count - 1;
      	end Get;

   	end CircularBuffer;

end Buffer;

-- use of package Buffer
-- we need a specific buffer CIrcularB because we need to reschedule not finished works
-- Put function insert a process into the buffer increment Count so that it shows that it is occupied
-- Get function read, so take out from the buffer and decrease Count so that it shows that there are free space