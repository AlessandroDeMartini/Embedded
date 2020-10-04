with Ada.Text_IO; use Ada.Text_IO;

-- Package: Semaphores
--
-- ==> Complete the code at the indicated places

-- s. is a value that represent the resources available
-- semaphors have 2 operators one increment and the other decrement the value of s. 
-- first is needed to inizialize the code with an int(Natural) >0 (part available to be used)
-- it is defined in the other file, as the max value.
-- define decrement that take out a point from the initial value
-- in the other file Count is initialized as the initial value, than we have to nicrement or decrement
-- if state > 0 start "Wait" it can be used, so update subracting 1
-- if the state is not full, so is less than the maximum, value can be incremented
-- (ho trovato la spiegazione di s. su wikipedia ma non mi torna, sembra un loop infinito!!!)
-- https://en.wikipedia.org/wiki/Semaphore_%28programming%29

package body Semaphores is
   
	protected body CountingSemaphore is
		
		entry Wait when Count > 0 is
			begin
    			Count := Count - 1;
    		end Wait;

    	entry Signal when Count < MaxCount is
    		begin
    			Count := Count + 1;	
    	end Signal;
    	
	end CountingSemaphore;
	
end Semaphores;
