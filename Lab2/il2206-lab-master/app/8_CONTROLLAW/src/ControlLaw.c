#include <stdio.h>
#include "system.h"
#include "includes.h"
#include "altera_avalon_pio_regs.h"
#include "sys/alt_irq.h"
#include "sys/alt_alarm.h"

#define DEBUG 1

#define HW_TIMER_PERIOD 100 /* 100ms */

/* Button Patterns */

#define GAS_PEDAL_FLAG      0x08
#define BRAKE_PEDAL_FLAG    0x04
#define CRUISE_CONTROL_FLAG 0x02
/* Switch Patterns */

#define TOP_GEAR_FLAG       0x00000002
#define ENGINE_FLAG         0x00000001

/* LED Patterns */

#define LED_RED_0 0x00000001 // Engine
#define LED_RED_1 0x00000002 // Top Gear

#define LED_RED_12 0x1000     // [2000m, 2400m]
#define LED_RED_13 0x2000     // [1600m, 2000m)
#define LED_RED_14 0x4000     // [1200m, 1600m)
#define LED_RED_15 0x8000     // [800m, 1200m)
#define LED_RED_16 0x10000    // [400m, 800m)
#define LED_RED_17 0x20000    // [0m, 400m)


#define LED_GREEN_0 0x0001    // Cruise Control activated
#define LED_GREEN_2 0x0004    // Cruise Control Button
#define LED_GREEN_4 0x0010    // Brake Pedal
#define LED_GREEN_6 0x0040    // Gas Pedal

/*
 * Definition of Tasks
 */

#define TASK_STACKSIZE 2048

OS_STK StartTask_Stack[TASK_STACKSIZE]; 
OS_STK ControlTask_Stack[TASK_STACKSIZE]; 
OS_STK VehicleTask_Stack[TASK_STACKSIZE];
OS_STK ButtonIO_Stack[TASK_STACKSIZE];
OS_STK SwitchIO_Stack[TASK_STACKSIZE];

// Task Priorities
 
#define STARTTASK_PRIO     5
#define VEHICLETASK_PRIO  10
#define CONTROLTASK_PRIO  12
#define BUTTONIOTASK_PRIO  13
#define SWITCHIOTASK_PRIO  14

// Task Periods

#define CONTROL_PERIOD  300
#define VEHICLE_PERIOD  300

/*
 * Definition of Kernel Objects 
*/

// Mailboxes
OS_EVENT *Mbox_Throttle;
OS_EVENT *Mbox_Velocity;
OS_EVENT *Mbox_Velocity_BUTTON;

OS_EVENT *Mbox_Brake;
OS_EVENT *Mbox_Cruise;
OS_EVENT *Mbox_Velocity_BUTTON;

OS_EVENT *Mbox_Engine;
OS_EVENT *Mbox_Gear;
OS_EVENT *Mbox_Gas;
//OS_EVENT *Mbox_Reset;

// Semaphores

OS_EVENT *VehicleTmrSem;
OS_EVENT *ControlTmrSem;
OS_EVENT *ButtonTmrSem;
OS_EVENT *SwitchTmrSem;

// SW-Timer

OS_TMR *VehicleTmr;
OS_TMR *ControlTmr; // Since they have the same period the callback fuction could be just one (Possiblility to add two other functions)

/*
 * Types
 */

enum active {on = 2, off = 1};

enum active gas_pedal = off;
enum active brake_pedal = off;
enum active top_gear = off;
enum active engine = off;
enum active cruise_control = off; 

/*
 * Global variables
 */

int delay; // Delay of HW-timer 
INT16U led_green = 0; // Green LEDs
INT32U led_red = 0;   // Red LEDs

int buttons_pressed(void)
{
  return ~IORD_ALTERA_AVALON_PIO_DATA(D2_PIO_KEYS4_BASE);    
}

int switches_pressed(void)
{
  return IORD_ALTERA_AVALON_PIO_DATA(DE2_PIO_TOGGLES18_BASE);    
}

void change_RED_led_status(int mask, int led_values)
{
  led_red = ((~mask) & led_red) | led_values;
  IOWR_ALTERA_AVALON_PIO_DATA(DE2_PIO_REDLED18_BASE, led_red);
}
void change_GREEN_led_status(int mask, int led_values)
{
  led_green = ((~mask) & led_green) | led_values;
  IOWR_ALTERA_AVALON_PIO_DATA(DE2_PIO_GREENLED9_BASE, led_green);
}

/*
 * ISR for HW Timer
 */

alt_u32 alarm_handler(void* context)
{
  OSTmrSignal(); /* Signals a 'tick' to the SW timers */
  return delay;
}

/* Timer Callback Functions */ 
void VehicleTmrCallback (void *ptmr, void *callback_arg)
{
  OSSemPost(VehicleTmrSem);
  printf("OSSemPost(VehicleTmr);\n");
}
void ControlTmrCallback (void *ptmr, void *callback_arg)
{
  OSSemPost(ControlTmrSem);
  OSSemPost(ButtonTmrSem);  // Same period, we don't need others timers 
  OSSemPost(SwitchTmrSem);  // Same period, we don't need others timers
  printf("OSSemPost(ControlTmr);\n");
}

static int b2sLUT[] = 
          {0x40, //0
		       0x79, //1
		       0x24, //2
		       0x30, //3
		       0x19, //4
		       0x12, //5
		       0x02, //6
		       0x78, //7
		       0x00, //8
		       0x18, //9
		       0x3F, //-
};

/*
 * convert int to seven segment display format
 */

int int2seven(int inval)
{
  return b2sLUT[inval];
}

/*
 * output current velocity on the seven segement display
 */

void show_velocity_on_sevenseg(INT8S velocity)
{
  int tmp = velocity;
  int out;
  INT8U out_high = 0;
  INT8U out_low = 0;
  INT8U out_sign = 0;

  if(velocity < 0)
  {
    out_sign = int2seven(10);
    tmp *= -1;
  }
  else
  {
    out_sign = int2seven(0);
  }

  out_high = int2seven(tmp / 10);
  out_low  = int2seven(tmp - (tmp/10) * 10);
  
  out = int2seven(0) << 21 |
            out_sign << 14 |
            out_high << 7  |
            out_low;
  IOWR_ALTERA_AVALON_PIO_DATA(DE2_PIO_HEX_LOW28_BASE,out);
}

/*
 * shows the target velocity on the seven segment display (HEX5, HEX4)
 * when the cruise control is activated (0 otherwise)
 */

void show_target_velocity(INT8U target_vel)
{
  int tmp = target_vel;
  int out;
  INT8U out_high = 0;
  INT8U out_low = 0;
  // INT8U out_sign = 0;

  // if(velocity < 0)
  // {
  //   out_sign = int2seven(10);
  //   tmp *= -1;
  // }
  // else
  // {
  //   out_sign = int2seven(0);
  // }

  out_high = int2seven(tmp / 10);
  out_low  = int2seven(tmp - (tmp/10) * 10);
  
  out = int2seven(0) << 21 |
            // out_sign << 14 |
            out_high << 7  |
            out_low;
  IOWR_ALTERA_AVALON_PIO_DATA(DE2_PIO_HEX_HIGH28_BASE,out);
}

/*
 * indicates the position of the vehicle on the track with the four leftmost red LEDs
 * LEDR17: [0m, 400m)
 * LEDR16: [400m, 800m)
 * LEDR15: [800m, 1200m)
 * LEDR14: [1200m, 1600m)
 * LEDR13: [1600m, 2000m)
 * LEDR12: [2000m, 2400m]
 */

void show_position(INT16U position)
{

  INT32U led_interested = 0x3F000; //mask --> which are the LEDS I'm interesting in

  if(position<400)
    led_red = LED_RED_17;
  else if(position<800)
    led_red = LED_RED_16;
  else if(position<1200)
    led_red = LED_RED_15;
  else if(position<1600)
    led_red = LED_RED_14;
  else if(position<2000)
    led_red = LED_RED_13;
  else if(position<=2400)
    led_red = LED_RED_12;

  change_RED_led_status(led_interested, led_red);

}

/*
 * The function 'adjust_position()' adjusts the position depending on the
 * acceleration and velocity.
 */

INT16U adjust_position(INT16U position, INT16S velocity,
		       INT8S acceleration, INT16U time_interval)
{
  INT16S new_position = position + velocity * time_interval / 1000;

  if (new_position > 2400) 
  {
    new_position -= 2400;
  } 
  else if (new_position < 0)
  {
    new_position += 2400;
  }

  show_position(new_position);
  return new_position;
}
 
/*
 * The function 'adjust_velocity()' adjusts the velocity depending on the
 * acceleration.
 */

INT16S adjust_velocity(INT16S velocity, INT8S acceleration,  
		       enum active brake_pedal, INT16U time_interval)
{
  INT16S new_velocity;
  INT8U brake_retardation = 200;

  if (brake_pedal == off)
    new_velocity = velocity  + (float) (acceleration * time_interval) / 1000.0;
  else 
  {
    if (brake_retardation * time_interval / 1000 > velocity)
      new_velocity = 0;
    else
      new_velocity = velocity - brake_retardation * time_interval / 1000;
  }
  return new_velocity;
}

/*
 * The task 'VehicleTask' updates the current velocity of the vehicle
 */

void VehicleTask(void* pdata)
{ 
  INT8U err;  
  void* msg;
  INT8U* throttle; 
  INT8S acceleration;  
  INT8S retardation;   
  INT16U position = 0; 
  INT16S velocity = 0; 
  INT16S wind_factor;   
  // enum active brake_pedal = off;

  printf("Vehicle task created!\n");

  while(1)
    {
      err = OSMboxPost(Mbox_Velocity, (void *) &velocity);
      err = OSMboxPost(Mbox_Velocity_BUTTON, (void *) &velocity);

      OSSemPend(VehicleTmrSem, 0, &err);

      /* Non-blocking read of mailbox: 
	    - message in mailbox: update throttle
	    - no message:         use old throttle
      */

      msg = OSMboxPend(Mbox_Throttle, 1, &err); 
      if (err == OS_NO_ERR) 
	      throttle = (INT8U*) msg;
      msg = OSMboxPend(Mbox_Brake, 1, &err); 
      if (err == OS_NO_ERR) 
	      brake_pedal = (enum active) msg; 

      // vehichle cannot effort more than 80 units of throttle
      if (*throttle > 80) *throttle = 80;

      // brakes + wind
      if (brake_pedal == off)
	    {
	      acceleration = (*throttle) - 1*velocity;

	      if (400 <= position && position < 800)
	        acceleration -= 2; // traveling uphill
	      else if (800 <= position && position < 1200)
	        acceleration -= 4; // traveling steep uphill
	      else if (1600 <= position && position < 2000)
	        acceleration += 4; //traveling downhill
	      else if (2000 <= position)
	        acceleration += 2; // traveling steep downhill
	    }
      else
	      acceleration = -4*velocity;

      printf("Position: %d m\n", position);
      printf("Velocity: %d m/s\n", velocity);
      printf("Accell: %d m/s2\n", acceleration);
      printf("Throttle: %d V\n", *throttle);

      // position = position + velocity * VEHICLE_PERIOD / 1000;
      velocity = velocity  + acceleration * VEHICLE_PERIOD / 1000.0;
      
      // if(position > 2400)
	    //   position = 0;

      position = adjust_position(position, velocity, acceleration, VEHICLE_PERIOD);
      // velocity = adjust_velocity(velocity, acceleration, brake_pedal, VEHICLE_PERIOD);

      show_velocity_on_sevenseg((INT8S) velocity);

    }
} 
 
/*
 * The task 'ControlTask' is the main task of the application. It reacts
 * on sensors and generates responses.
 */

void ControlTask(void* pdata)
{
  INT8U err;
  INT8U throttle = 0; /* Value between 0 and 80, which is interpreted as between 0.0V and 8.0V */
  void* msg;
  INT16S* current_velocity;
  INT16S* cruise_velocity;

  // Store imputs from the swetches;
  int gas_pedal_tmp=0;
  int top_gear_tmp=0;
  int engine_tmp=0;
  int cruise_control_tmp=0;

  printf("Control Task created!\n");

  while(1)
    {
      msg = OSMboxPend(Mbox_Velocity, 0, &err);
      if (err== OS_NO_ERR)
        current_velocity = (INT16S*) msg;
      

      // Use green led to indicate cruise is on
      change_GREEN_led_status(0x1, (cruise_control == on)*0xff & LED_GREEN_0);

      // Se ho il cruise attivo prendo la velocità desiderata che rimane fissa a +- 4m/s
      
      /*
      if(*current_velocity < 25 || cruise_control == off)
      {
        cruise_control == off;
        show_velocity_on_sevenseg(0);
        
      }

      if (cruise_control == on)
      {
        printf("hey");
        show_velocity_on_sevenseg(*cruise_velocity);
        //led_green = LED_GREEN_0;
        //IOWR_ALTERA_AVALON_PIO_DATA(DE2_PIO_GREENLED9_BASE, LED_GREEN_0);
        
        if(*cruise_velocity >= 25) // What happen between 20 and 25
        {
          if( (*cruise_velocity - *current_velocity) > 4 )
          {
            throttle = throttle + 1;
          }
          if( (*cruise_velocity - *current_velocity) < 4 )
          {
            throttle = throttle - 1;
          }

        }
      }
      */

      err = OSMboxPost(Mbox_Throttle, (void *) &throttle);
      
      // OSTimeDlyHMSM(0,0,0, CONTROL_PERIOD);
      OSSemPend(ControlTmrSem, 0, &err);
    }
}


 /* The task ButtonIOTask permits to recive inpurs from buttons
  * generating responses
  */


void ButtonIOTask(void* pdata)
{
  INT8U err;
  int ButtonState;
  
  void* msg;
  INT16S* current_velocity = 0;

  printf ("ButtonIO Task created!\n");

  while (1)
  {
    ButtonState = buttons_pressed(); // 1,2,3 considering how many switches are on
    ButtonState = (ButtonState) & 0xf;
    
    msg = OSMboxPend(Mbox_Velocity_BUTTON, 0, &err);
    current_velocity = (INT16S*) msg;

    // err = OSMboxPost(Mbox_Gas, button_register & GAS_PEDAL_FLAG? on : off);
    // err = OSMboxPost(Mbox_Cruise,button_register & CRUISE_CONTROL_FLAG? on : off);
    // err = OSMboxPost(Mbox_Brake, button_register & BRAKE_PEDAL_FLAG? on : off);

      switch (ButtonState)
      {
        case CRUISE_CONTROL_FLAG:   // Key1 is pressed

          if(top_gear == on && *current_velocity >= 25)
          {
            
            printf( "CRUISE_CONTROL_FLAG \n");
            cruise_control = on;    // start cruise control 

            err = OSMboxPost(Mbox_Cruise, (void *) &current_velocity);
            //err = OSMboxPost(Mbox_Cruise_DISP, (void *) &current_velocity);
            
            //IOWR_ALTERA_AVALON_PIO_DATA(DE2_PIO_GREENLED9_BASE, LED_GREEN_2);
            
            change_GREEN_led_status(0x7E, LED_GREEN_2);
            printf("Cruise_velocity SENT: %d \n", *current_velocity );
          }

        break;
        case BRAKE_PEDAL_FLAG:      // Key2 is pressed
          
            printf( "BRAKE_PEDAL_FLAG \n");
            brake_pedal = on;       // start brake    
            cruise_control = off;   // cruise off   
            // IOWR_ALTERA_AVALON_PIO_DATA(DE2_PIO_GREENLED9_BASE, LED_GREEN_4); 
            change_GREEN_led_status(0x7E, LED_GREEN_4);
        break;
        case GAS_PEDAL_FLAG:        // Key3 is pressed
            printf( "GAS_PEDAL_FLAG \n");
            gas_pedal = on;               // start gas      
            cruise_control = off;   // cruise off
            
            // IOWR_ALTERA_AVALON_PIO_DATA(DE2_PIO_GREENLED9_BASE, LED_GREEN_6); 

            change_GREEN_led_status(0x7E, LED_GREEN_6);
        break;
        default:
          gas_pedal   = off;
          brake_pedal = off;
          if(cruise_control == on)
          {
            change_GREEN_led_status(0x1, LED_GREEN_0);
          }
          else
          {
            change_GREEN_led_status(0x1, NULL);
          }
          
            printf("Default state: led, cruise, break, gas remain equals \n");
        break;
      }
      
      
      OSSemPend(ButtonTmrSem, 0, &err);
  }   
}

void SwitchIOTask(void* pdata)
{
  int SwitchState;
  INT8U err;
  printf ("SwitchIO Task created!\n");
  INT32U led_interested = 0x3;
   while (1)
   {
      SwitchState = (~SwitchState) & 0xf;
      SwitchState = switches_pressed(); // 1,2,3 considering how many switches are on

      switch (SwitchState)
      {
        case ENGINE_FLAG:                // Switch0 is pressed
          
          printf ("ENGINE_FLAG \n");
          engine = on;                   // engine on 
          cruise_control = off;
          led_red = LED_RED_0;

          change_RED_led_status(led_interested, led_red);
          // change_GREEN_led_status(0x1, NULL);

        break;
        case TOP_GEAR_FLAG:             // Switch1 is pressed

          printf ("TOP_GEAR_FLAG \n");
          top_gear = on;      
          led_red = LED_RED_1;
          change_RED_led_status(led_interested,led_red);
  
          //IOWR_ALTERA_AVALON_PIO_DATA(DE2_PIO_REDLED18_BASE, LED_RED_1);    

        break;
        case TOP_GEAR_FLAG+ENGINE_FLAG:             // Switch1 is pressed

          printf ("TOP_GEAR_FLAG + ENGINE_FLAG \n");
          top_gear = on;
          engine = on;         

          led_red = LED_RED_1 + LED_RED_0;
          change_RED_led_status(led_interested,led_red);

          //IOWR_ALTERA_AVALON_PIO_DATA(DE2_PIO_REDLED18_BASE, LED_RED_1 + LED_RED_0);    
           
        break;
        default:

          engine = off;
          top_gear = off;
          change_GREEN_led_status(0x1, NULL);
          change_RED_led_status(led_interested, NULL);

          printf("Default state: engine, top_gear off \n");

        break;
      }
     OSSemPend(SwitchTmrSem, 0, &err);
   }
}
    
/* 
 * The task 'StartTask' creates all other tasks kernel objects and
 * deletes itself afterwards.
 */ 

void StartTask(void* pdata)
{
  INT8U err;
  void* context;

  static alt_alarm alarm;     /* Is needed for timer ISR function */
  
  /* Base resolution for SW timer : HW_TIMER_PERIOD ms */
  delay = alt_ticks_per_second() * HW_TIMER_PERIOD / 1000; 
  printf("delay in ticks %d\n", delay);

  /* 
   * Create Hardware Timer with a period of 'delay' 
   */
  if (alt_alarm_start (&alarm,
		                    delay,
		                    alarm_handler,
		                    context) < 0)
  {
    printf("No system clock available!n");
  }

  /* 
   * Create Software Timers
   */

   //Create VehicleTask Timer
   VehicleTmr = OSTmrCreate(0, //delay
                          VEHICLE_PERIOD/HW_TIMER_PERIOD, //period
                          OS_TMR_OPT_PERIODIC,
                          VehicleTmrCallback, //OS_TMR_CALLBACK
                          (void *)0,
                          "VehicleTmr",
                          &err);
                            
   if (DEBUG) 
   {
    if (err == OS_ERR_NONE) 
    { //if creation successful
      printf("VehicleTmr created\n");
    }
   }
   
   //Create ControlTask Timer
   ControlTmr = OSTmrCreate(0, //delay
                            CONTROL_PERIOD/HW_TIMER_PERIOD, //period
                            OS_TMR_OPT_PERIODIC,
                            ControlTmrCallback, //OS_TMR_CALLBACK
                            (void *)0,
                            "ControlTmr",
                            &err);
                            
   if (DEBUG) 
   {
    if (err == OS_ERR_NONE) 
    { //if creation successful
      printf("ControlTmr created\n");
    }
   }

  /* 
   * Start Software Timers
   */

  //start VehicleTask Timer
  OSTmrStart(VehicleTmr, &err);
   
  if (DEBUG) 
  {
    if (err == OS_ERR_NONE) 
    { //if start successful
      printf("VehicleTmr started\n");
    }
  }

  //start ControlTask Timer
  OSTmrStart(ControlTmr, &err);
   
  if (DEBUG) 
  {
    if (err == OS_ERR_NONE) 
    { //if start successful
      printf("ControlTmr started\n");
    }
  } 

  /*
   * Creation of Kernel Objects
   */
  
  VehicleTmrSem = OSSemCreate(0);   
  ControlTmrSem = OSSemCreate(0); 
  ButtonTmrSem  = OSSemCreate(0);
  SwitchTmrSem  = OSSemCreate(0);

  // Mailboxes
  Mbox_Throttle = OSMboxCreate((void*) 0); /* Empty Mailbox - Throttle */
  Mbox_Velocity = OSMboxCreate((void*) 0); /* Empty Mailbox - Velocity */
  Mbox_Brake    = OSMboxCreate((void*) 0); /* Empty Mailbox - Brake */
  Mbox_Cruise   = OSMboxCreate((void*) 0); /* Empty Mailbox - Cruise */
  //Mbox_Cruise_DISP = OSMboxCreate((void*) 0);
  Mbox_Velocity_BUTTON = OSMboxCreate((void*) 0);
  Mbox_Gas = OSMboxCreate((void*) 0);
  Mbox_Gear = OSMboxCreate((void*) 0); /* Empty Mailbox -  Gear*/
  Mbox_Engine = OSMboxCreate((void*) 0); 
  /*
    * Create statistics task
  */

  OSStatInit();

  /* 
   * Creating Tasks in the system 
   */


  err = OSTaskCreateExt(
			ControlTask, // Pointer to task code
			NULL,        // Pointer to argument that is
			// passed to task
			&ControlTask_Stack[TASK_STACKSIZE-1], // Pointer to top
			// of task stack
			CONTROLTASK_PRIO,
			CONTROLTASK_PRIO,
			(void *)&ControlTask_Stack[0],
			TASK_STACKSIZE,
			(void *) 0,
			OS_TASK_OPT_STK_CHK);

  err = OSTaskCreateExt(
			VehicleTask, // Pointer to task code
			NULL,        // Pointer to argument that is
			// passed to task
			&VehicleTask_Stack[TASK_STACKSIZE-1], // Pointer to top
			// of task stack
			VEHICLETASK_PRIO,
			VEHICLETASK_PRIO,
			(void *)&VehicleTask_Stack[0],
			TASK_STACKSIZE,
			(void *) 0,
			OS_TASK_OPT_STK_CHK);
  
  err = OSTaskCreateExt(
			ButtonIOTask, // Pointer to task code
			NULL,        // Pointer to argument that is
			// passed to task
			&ButtonIO_Stack[TASK_STACKSIZE-1], // Pointer to top
			// of task stack
			BUTTONIOTASK_PRIO,
			BUTTONIOTASK_PRIO,
			(void *)&ButtonIO_Stack[0],
			TASK_STACKSIZE,
			(void *) 0,
			OS_TASK_OPT_STK_CHK);

  err = OSTaskCreateExt(
			SwitchIOTask, // Pointer to task code
			NULL,        // Pointer to argument that is
			// passed to task
			&SwitchIO_Stack[TASK_STACKSIZE-1], // Pointer to top
			// of task stack
			SWITCHIOTASK_PRIO,
			SWITCHIOTASK_PRIO,
			(void *)&SwitchIO_Stack[0],
			TASK_STACKSIZE,
			(void *) 0,
			OS_TASK_OPT_STK_CHK);

  printf("All Tasks and Kernel Objects generated!\n");

  /* Task deletes itself */

  OSTaskDel(OS_PRIO_SELF);
}

/*
 *
 * The function 'main' creates only a single task 'StartTask' and starts
 * the OS. All other tasks are started from the task 'StartTask'.
 *
 */

int main(void) 
{

  printf("Lab: Cruise Control\n");
 
  OSTaskCreateExt(
		  StartTask, // Pointer to task code
		  NULL,      // Pointer to argument that is
		  // passed to task
		  (void *)&StartTask_Stack[TASK_STACKSIZE-1], // Pointer to top
		  // of task stack 
		  STARTTASK_PRIO,
		  STARTTASK_PRIO,
		  (void *)&StartTask_Stack[0],
		  TASK_STACKSIZE,
		  (void *) 0,  
		  OS_TASK_OPT_STK_CHK | OS_TASK_OPT_STK_CLR);
	 
  OSStart();
  
  return 0;
}
