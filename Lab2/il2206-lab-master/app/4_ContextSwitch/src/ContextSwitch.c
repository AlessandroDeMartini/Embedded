// File: TwoTasks.c 

#include <stdio.h>
#include <string.h>
#include <inttypes.h> //printf with32u
#include "includes.h"
#include "altera_avalon_pio_regs.h"
#include "altera_avalon_performance_counter.h" //insert library needed by counter
#include "sys/alt_irq.h"
#include "sys/alt_alarm.h"
#include "system.h"

#define DEBUG 0
#define SEM   1
#define HW_TIMER_PERIOD 100

/* Definition of Task Stacks */
/* Stack grows from HIGH to LOW memory */
#define   TASK_STACKSIZE       2048
OS_STK    task1_stk[TASK_STACKSIZE];
OS_STK    task2_stk[TASK_STACKSIZE];
OS_STK    stat_stk[TASK_STACKSIZE];
OS_EVENT  *DispSem1;
OS_EVENT  *DispSem2;

/* Definition of Task Priorities */
#define TASK1_PRIORITY      6  // highest priority
#define TASK2_PRIORITY      7
#define TASK_STAT_PRIORITY 12  // lowest priority 

// ((void *)PERFORMANCE_COUNTER_BASE) p  //definition peripheral's HW base adress
 //probably it is not written in the right way 


void printStackSize(char* name, int prio) 
{
  INT8U err; // INT8U
  OS_STK_DATA stk_data;   /* Stack definition */
    
  OSTaskStkChk(prio, &stk_data);   /* It determines a task's stack statistic, it computes the amount of free stack space */

  if (err == OS_NO_ERR) 
  {  /* sucessful function */
    if (DEBUG == 1)
       printf("%s (priority %d) - Used: %" PRIu32 "; Free: %" PRIu32 "\n", 
	            name, prio, stk_data.OSUsed, stk_data.OSFree); /* .OSUsed -> Used space in stack
                                                         .OSFree -> Free space in stack */
  }
  else
    {
      if (DEBUG == 1)
	      printf("Stack Check Error!\n");    
    }
}

/* Prints a message and sleeps for given time interval */
void task1(void* pdata)
{
    INT8U err;
    char state = '0';

    while (1)
    { 
      printf("Task 0 - State %s \n", state);

      OSSemPost(DispSem2); // Semaphore is signaled 
      PERF_RESET( PERFORMANCE_COUNTER_BASE );   //reset of the counter 
      
      PERF_START_MEASURING( PERFORMANCE_COUNTER_BASE );  //start the counter  when the semaphores say to wait
      OSSemPend(DispSem1, 0, &err); // Semaphore is waiting
        
      if (state == '0')
          state = '1';
      else
          state = '0';  
      
      OSTimeDlyHMSM(0, 0, 0, 1); // 1ms delay
      
      /* Context Switch to next task
		    * Task will go to the ready state
		    * after the specified delay
		  */

    }
}

/* Prints a message and sleeps for given time interval */
void task2(void* pdata)
{
    INT8U err;
    char state = '0';
    int clock_cycles = 0;
    long double ContextSwitch_Seconds = 0;

    int count = 0;
    long double ContextSwitchAverage = 0;
    long double ContextSwitchAccumulator=0;

    while (1)
    { 
        // PERF_STOP_MEASURING( PERFORMANCE_COUNTER_BASE ); //stop the counter when the semaphores say to start next task
        
      OSSemPend(DispSem2, 0, &err); // semaphore is waiting 

      PERF_STOP_MEASURING( PERFORMANCE_COUNTER_BASE ); //stop the counter when the semaphores say to start next task

      clock_cycles = perf_get_total_time( (void *) PERFORMANCE_COUNTER_BASE ); // alt_u64 number;
      ContextSwitch_Seconds  = (long double) clock_cycles/ 50 ;

      if(ContextSwitchAverage == 0 || ContextSwitch_Seconds < ContextSwitchAverage*1.5)
      {
        ContextSwitchAccumulator = ContextSwitchAccumulator + ContextSwitch_Seconds;
        count = count + 1;
        ContextSwitchAverage = ContextSwitchAccumulator/ (double) count;
      }
        
      printf( "total time Context Switch: %Lf 1e-6 seconds; \n", ContextSwitch_Seconds);

      if(count >= 11)
        printf( "total time Context Switch, AVARAGE: %Lf 1e-6 seconds; \n", ContextSwitch_Seconds);

      printf("Task 1 - state %s \n",state);

      if (state == '0')
          state = '1';
      else
          state = '0';
      
      
      OSSemPost(DispSem1);  // Semaphore is signaled
      OSTimeDlyHMSM(0, 0, 0, 1);  // 1ms delay

    }
}

/* Printing Statistics */
void statisticTask(void* pdata)
{
  while(1)
    {
      printStackSize("Task1", TASK1_PRIORITY);
      printStackSize("Task2", TASK2_PRIORITY);
      printStackSize("StatisticTask", TASK_STAT_PRIORITY);
    }
}

/* The main function creates two task and starts multi-tasking */
int main(void)
{
    
  printf("Lab 3 - Two Tasks\n");
  
  if (SEM == 1){
    DispSem1 = OSSemCreate(1); // Binary Semaphore created
    DispSem2 = OSSemCreate(1);
  }

  OSTaskCreateExt  /* Create a task menaged by microC */
    ( task1,                        // Pointer to task code
      NULL,                         // Pointer to argument passed to task
      &task1_stk[TASK_STACKSIZE-1], // Pointer to top of task stack - ultimo elemento (conto da zero)
      TASK1_PRIORITY,               // Desired Task priority
      TASK1_PRIORITY,               // Task ID
      &task1_stk[0],                // Pointer to bottom of task stack - primo elemento (conto da zero)
      TASK_STACKSIZE,               // Stacksize
      NULL,                         // Pointer to user supplied memory (not needed)
      OS_TASK_OPT_STK_CHK |         // Stack Checking enabled 
      OS_TASK_OPT_STK_CLR           // Stack Cleared                                 
      );
	   
  OSTaskCreateExt   /* Create a task menaged by microC */  
    ( task2,                        // Pointer to task code
      NULL,                         // Pointer to argument passed to task
      &task2_stk[TASK_STACKSIZE-1], // Pointer to top of task stack
      TASK2_PRIORITY,               // Desired Task priority
      TASK2_PRIORITY,               // Task ID
      &task2_stk[0],                // Pointer to bottom of task stack
      TASK_STACKSIZE,               // Stacksize
      NULL,                         // Pointer to user supplied memory (not needed)
      OS_TASK_OPT_STK_CHK |         // Stack Checking enabled 
      OS_TASK_OPT_STK_CLR           // Stack Cleared                       
      );  

  if (DEBUG == 1)
    {
      OSTaskCreateExt
	                  ( statisticTask,                // Pointer to task code
	                    NULL,                         // Pointer to argument passed to task
	                    &stat_stk[TASK_STACKSIZE-1],  // Pointer to top of task stack
	                    TASK_STAT_PRIORITY,           // Desired Task priority
	                    TASK_STAT_PRIORITY,           // Task ID
	                    &stat_stk[0],                 // Pointer to bottom of task stack
	                    TASK_STACKSIZE,               // Stacksize
	                    NULL,                         // Pointer to user supplied memory (not needed)
	                    OS_TASK_OPT_STK_CHK |         // Stack Checking enabled 
	                    OS_TASK_OPT_STK_CLR           // Stack Cleared                              
	                  );
    }  

  OSStart();
  return 0;
}
