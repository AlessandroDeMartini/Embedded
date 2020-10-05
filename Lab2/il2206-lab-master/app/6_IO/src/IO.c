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

#define LED_GREEN_0 0x0001 // Cruise Control activated
#define LED_GREEN_2 0x0002 // Cruise Control Button
#define LED_GREEN_4 0x0010 // Brake Pedal
#define LED_GREEN_6 0x0040 // Gas Pedal

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
OS_EVENT *Mbox_Brake;

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

/*
 * ISR for HW Timer
 */
alt_u32 alarm_handler(void* context)
{
  OSTmrSignal(); /* Signals a 'tick' to the SW timers */
  return delay;
}

/* Timer Callback Functions */ 
void VehicleTmrCallback (void *ptmr, void *callback_arg){
  OSSemPost(VehicleTmrSem);
  printf("OSSemPost(VehicleTmr);\n");
}
void ControlTmrCallback (void *ptmr, void *callback_arg){
  OSSemPost(ControlTmrSem);
  OSSemPost(ButtonTmrSem);  // Same period, we don't need others timers 
  OSSemPost(SwitchTmrSem);  // Same period, we don't need others timers
  printf("OSSemPost(ControlTmr);\n");
}

static int b2sLUT[] = {0x40, //0
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
int int2seven(int inval){
  return b2sLUT[inval];
}

/*
 * output current velocity on the seven segement display
 */
void show_velocity_on_sevenseg(INT8S velocity){
  int tmp = velocity;
  int out;
  INT8U out_high = 0;
  INT8U out_low = 0;
  INT8U out_sign = 0;

  if(velocity < 0){
    out_sign = int2seven(10);
    tmp *= -1;
  }else{
    out_sign = int2seven(0);
  }

  out_high = int2seven(tmp / 10);
  out_low = int2seven(tmp - (tmp/10) * 10);
  
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
}

/*
 * The function 'adjust_position()' adjusts the position depending on the
 * acceleration and velocity.
 */
INT16U adjust_position(INT16U position, INT16S velocity,
		       INT8S acceleration, INT16U time_interval)
{
  INT16S new_position = position + velocity * time_interval / 1000;

  if (new_position > 2400) {
    new_position -= 2400;
  } else if (new_position < 0){
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
  else {
    if (brake_retardation * time_interval / 1000 > velocity)
      new_velocity = 0;
    else
      new_velocity = velocity - brake_retardation * time_interval / 1000;
  }
  
  return new_velocity;
}

INT16S velocity = 0; 

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
  enum active brake_pedal = off;

  printf("Vehicle task created!\n");

  while(1)
    {
      err = OSMboxPost(Mbox_Velocity, (void *) &velocity);

      // OSTimeDlyHMSM(0,0,0,VEHICLE_PERIOD); 
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

      position = position + velocity * VEHICLE_PERIOD / 1000;
      velocity = velocity  + acceleration * VEHICLE_PERIOD / 1000.0;
      
      if(position > 2400)
	      position = 0;

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
  INT8U throttle = 40; /* Value between 0 and 80, which is interpreted as between 0.0V and 8.0V */
  void* msg;
  INT16S* current_velocity;

  printf("Control Task created!\n");

  while(1)
    {
      msg = OSMboxPend(Mbox_Velocity, 0, &err);
      current_velocity = (INT16S*) msg;

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
  int ButtonState;
  INT8U err;
  printf ("ButtonIO Task created!\n");

  while (1)
  {
      ButtonState = buttons_pressed();
      ButtonState = ButtonState & 0xf; // TRANSFORM IN 8 BIT LONG
      switch (ButtonState)
      {
        case CRUISE_CONTROL_FLAG:   // Key1 is pressed
          // IF check constraint
          printf( "CRUISE_CONTROL_FLAG \n");

          if(top_gear == on && velocity >= 20)
          {
            // cheack for velocity is necessery -> cannot activate cruise control if v < 20
            printf( "Cruise_control, velocity check: %d \n", velocity);  //to be delated and inserted in the if cycle below
            
            cruise_control = on;    // start cruise control 
            led_green = LED_GREEN_2;

          }
        break;

        case BRAKE_PEDAL_FLAG:      // Key2 is pressed
            printf( "CRUISE_CONTROL_FLAG \n");
            brake_pedal = on;       // start brake    
            cruise_control = off;   // cruise off     
            led_green = LED_GREEN_4;
          break;
        
        case GAS_PEDAL_FLAG:        // Key3 is pressed
            printf( "CRUISE_CONTROL_FLAG \n");
            gas_pedal = on;               // start gas      
            cruise_control = off;   // cruise off     
            led_green = LED_GREEN_6;
          break;
    
        default:
          gas_pedal   = off;
          brake_pedal = off;
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

   while (1)
   {
      SwitchState = switches_pressed();
      SwitchState = SwitchState & 0xf;

      printf ("SwitchState: %d \n", SwitchState);

      switch (SwitchState)
      {
        case ENGINE_FLAG:                // Switch0 is pressed
          
          printf ("ENGINE_FLAG \n");
          engine = on;                   // engine on      
          led_red = LED_RED_0;

          break;
        case TOP_GEAR_FLAG:             // Switch1 is pressed

          printf ("TOP_GEAR_FLAG");
          top_gear = on;         
          led_red = LED_RED_1;
          
          break;
        default:
          engine = off;
          top_gear = off;
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
                            
   if (DEBUG) {
    if (err == OS_ERR_NONE) { //if creation successful
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
                            
   if (DEBUG) {
    if (err == OS_ERR_NONE) { //if creation successful
      printf("ControlTmr created\n");
    }
   }

  /* 
   * Start Software Timers
   */

  //start VehicleTask Timer
  OSTmrStart(VehicleTmr, &err);
   
  if (DEBUG) {
    if (err == OS_ERR_NONE) { //if start successful
      printf("VehicleTmr started\n");
    }
  }

  //start ControlTask Timer
  OSTmrStart(ControlTmr, &err);
   
  if (DEBUG) {
    if (err == OS_ERR_NONE) { //if start successful
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
  Mbox_Brake = OSMboxCreate((void*) 0); /* Empty Mailbox - Velocity */
   
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

int main(void) {

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
