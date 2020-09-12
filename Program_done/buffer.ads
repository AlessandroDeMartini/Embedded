package Buffer is

   Size: constant Integer := 3;
   subtype Item is Integer;
   type Index is mod Size;
   type Item_Array is array(Index) of Item;

   protected type CircularBuffer is
      entry Put(X: in Item);
      entry Get(X: out Item);
   private
      A: Item_Array;
      In_Ptr, Out_Ptr: Index := 0;
      Count: Integer range 0..Size := 0;
   end CircularBuffer;

end Buffer;

-- buffer have size 3 because each producer need to enter 1 process
-- it contain integer value
-- index is a integer positive (modular) value that goes from 0 to n-1 size
-- A is an item array