#include <stdio.h>
#include "system.h"
#include "includes.h"
#include "altera_avalon_pio_regs.h"
#include "sys/alt_irq.h"
#include "sys/alt_alarm.h"
#include "altera_avalon_performance_counter.h"
#include "alt_types.h"

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
OS_STK ButtonIOTask_Stack[TASK_STACKSIZE];
OS_STK SwitchIOTask_Stack[TASK_STACKSIZE];
OS_STK OverloadDetectionTask_Stack[TASK_STACKSIZE];
OS_STK WatchdogTask_Stack[TASK_STACKSIZE];
OS_STK ExtraLoadTask_Stack[TASK_STACKSIZE];

// Task Priorities
#define STARTTASK_PRIO     2
#define WATCHDOGTASK_PRIO  4
#define VEHICLETASK_PRIO  6
#define SWITCHIOTASK_PRIO  7
#define BUTTONIOTASK_PRIO  8
#define CONTROLTASK_PRIO  12
#define EXTRALOADTASK_PRIO 13
#define OVERLOADDETECTIONTASK_PRIO  16


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
OS_EVENT *Mbox_Gas;
OS_EVENT *Mbox_Cruise;
OS_EVENT *Mbox_Engine;
OS_EVENT *Mbox_Gear;
OS_EVENT *Mbox_Reset;

// Semaphores
OS_EVENT *ControlTimerSem;
OS_EVENT *VehicleTimerSem;
OS_EVENT *ButtonTaskTimerSem;
OS_EVENT *SwitchTaskTimerSem;
OS_EVENT *OverloadDetectionTaskTimerSem;
OS_EVENT *WatchdogTaskTimerSem;
OS_EVENT *ExtraLoadTaskTimerSem;

// SW-Timer
OS_TMR *ControlTimer;
OS_TMR *VehicleTimer;

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

int OK = 1; //overload signal if 1 no overload detected, if 0 overload detected

int buttons_pressed(void)
{
  return ~IORD_ALTERA_AVALON_PIO_DATA(D2_PIO_KEYS4_BASE);
}

int switches_pressed(void)
{
  return IORD_ALTERA_AVALON_PIO_DATA(DE2_PIO_TOGGLES18_BASE);
}

/*
Function for updating the green leds near the buttons
*/
void change_led_button_status(int mask, int led_values)
{
  led_green=((~mask) & led_green) | led_values;
  IOWR_ALTERA_AVALON_PIO_DATA(DE2_PIO_GREENLED9_BASE,led_green);
}

/*
Function for updating the red leds near the swithces
mas--> it's needed to select the which led has to be update so the other led's are note
      influenced
*/
void change_led_switch_status(int mask, int led_values)
{
  led_red=((~mask) & led_red) | led_values;
  //int currentLedStatus = IORD_ALTERA_AVALON_PIO_DATA(DE2_PIO_REDLED18_BASE); Old Implementation made by reading the led status instead of using global variable
  IOWR_ALTERA_AVALON_PIO_DATA(DE2_PIO_REDLED18_BASE, led_red);
}


/*
 * ISR for HW Timer
 */
alt_u32 alarm_handler(void* context)
{
  OSTmrSignal(); /* Signals a 'tick' to the SW timers */

  return delay;
}

/* Callback Functions */
void VehicleTimerCallback (void *ptmr, void *callback_arg){
  OSSemPost(VehicleTimerSem);
  printf("OSSemPost(VehicleTimerSem);\n");
}

void ControlTimerCallback (void *ptmr, void *callback_arg){
  INT8U err;
  OSSemPost(ControlTimerSem);
  //The post is made on all the follow sempaphores because they all have to execute
  //with the same period of the ControlTask
  OSSemPost(ButtonTaskTimerSem);
  OSSemPost(SwitchTaskTimerSem);
  OSSemPost(WatchdogTaskTimerSem);
  //reset of semaphore of Overload in case the task didn't had the possibility
  //to do the pend in the past hyperperiod
  OSSemSet(OverloadDetectionTaskTimerSem,0,&err);
  OSSemPost(OverloadDetectionTaskTimerSem);
  //reset of semaphore of Extraload in case the task didn't had the possibility
  //to do the pend in the past hyperperiod
  OSSemSet(ExtraLoadTaskTimerSem,0,&err);
  OSSemPost(ExtraLoadTaskTimerSem);
  printf("OSSemPost(Control Timers);\n");
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
  int tmp = target_vel;
  int out;
  INT8U out_high = 0;
  INT8U out_low = 0;
            ///INT8U out_sign = 0; CHECK IF WORK WITH IT COMMENTED

            /*if(target_vel < 0){
              out_sign = int2seven(10);
              tmp *= -1;
            }else{
              out_sign = int2seven(0);
            }*/

  out_high = int2seven(tmp / 10);
  out_low = int2seven(tmp - (tmp/10) * 10);

  out = int2seven(0) << 21 |
            //  out_sign << 14 |
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
  INT32U ledStatus=0;
  if(position<400){
    ledStatus=0x20000;
  }else if(position<800){
    ledStatus=0x10000;
  }else if(position<1200){
    ledStatus=0x8000;
  }else if(position<1600){
    ledStatus=0x4000;
  }else if(position<2000){
    ledStatus=0x2000;
  }else if(position<=2400){
    ledStatus=0x1000;
  }
  INT32U mask= 0x3F000;
  change_led_switch_status(mask,ledStatus);
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
  //enum active brake_pedal = off;---IT WAS OVERLAPPING THE EXTERNAL VARIABLE

  printf("Vehicle task created!\n");

  while(1)
    {
      err = OSMboxPost(Mbox_Velocity, (void *) &velocity);
      //OSTimeDlyHMSM(0,0,0,VEHICLE_PERIOD);
      OSSemPend(VehicleTimerSem, 0, &err);

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

      //position = position + velocity * VEHICLE_PERIOD / 1000;
      velocity = velocity  + acceleration * VEHICLE_PERIOD / 1000.0;
      //if(position > 2400)
	    //  position = 0;
      position = adjust_position(position, velocity, acceleration, VEHICLE_PERIOD);

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
  //Variables for temporarly storing the input from button and switches
  int gas_pedal_tmp=0;
  int top_gear_tmp=0;
  int engine_tmp=0;
  int cruise_control_tmp=0;
  // Variables for the PID controller
  INT16S target_velocity=0;
  int error_accumulator =0;
  int error_previous = 0;
  int error_velocity = 0;

  printf("Control Task created!\n");

  while(1)
    {
      //Read of input data
      msg = OSMboxPend(Mbox_Velocity, 0, &err);
      current_velocity = (INT16S*) msg;

      msg = OSMboxPend(Mbox_Gas, 1, &err);
      if (err == OS_NO_ERR)
        gas_pedal_tmp = (INT8U*) msg;
      msg = OSMboxPend(Mbox_Gear, 1, &err);
      if (err == OS_NO_ERR)
        top_gear_tmp = (INT8U*) msg;
      msg = OSMboxPend(Mbox_Engine, 1, &err);
      if (err == OS_NO_ERR)
        engine_tmp = (INT8U*) msg;
      msg = OSMboxPend(Mbox_Cruise, 1, &err);
      if (err == OS_NO_ERR)
        cruise_control_tmp = (INT8U*) msg;

      //Change of the status based on the input data and on the constraints
      if(engine_tmp == on && engine==off)
        engine = on;
      else if(engine_tmp == off && *current_velocity == 0)
        engine = off;
      gas_pedal = (gas_pedal_tmp==on && engine ==on)?on:off;
      top_gear = top_gear_tmp;
      if(cruise_control==off){
        cruise_control=cruise_control_tmp==on
        && top_gear==on
        && *current_velocity>20
        && brake_pedal==off
        && gas_pedal==off
        && engine == on
        ? on :off;
      }else{
        cruise_control=cruise_control==on
        && top_gear==on
        && *current_velocity>20
        && brake_pedal==off
        && gas_pedal==off
        && engine == on
        ? on :off;
      }

      //Use the green LED LEDG0 to indicate that the cruise control is active
      change_led_button_status( 0x1, (cruise_control == on)*0xff &  LED_GREEN_0);

      //Evaluation of the throttle
      if(cruise_control == on){
        if(target_velocity == 0)
        {
          target_velocity = *current_velocity;
          error_accumulator = 0;
          error_previous = 0;
          show_target_velocity(target_velocity);
        }
        error_velocity=target_velocity-*current_velocity;
        error_accumulator = error_accumulator+error_velocity;
        throttle = error_velocity*5+
                    error_accumulator*CONTROL_PERIOD/1000*3+
                    (error_velocity-error_previous)/CONTROL_PERIOD/1000*2;

        error_previous = error_velocity;
        printf("CONTROLLED THROTTLE %d\n",throttle );
        printf("ERROR VELOCITY%d\n",error_velocity);

      }else if(gas_pedal == on){
        throttle=40;
        target_velocity = 0;
        show_target_velocity(0);

      }else{
        throttle=0;
        target_velocity = 0;
        show_target_velocity(0);

      }

      //printf("Throttle value is %d \n",throttle );

      err = OSMboxPost(Mbox_Throttle, (void *) &throttle);

      //OSTimeDlyHMSM(0,0,0, CONTROL_PERIOD);
      OSSemPend(ControlTimerSem, 0, &err);
    }
}

/*
  * The task 'ButtonIOTask' read the data from the buttons and manage the leds linked
  * to them
  */
void ButtonIOTask(void* pdata)
{
  INT8U err;
  int button_register;
  printf("ButtonIO Task created!\n");

  while(1)
  {
    //Read the input data
    button_register = (~button_register) & 0xf;
    button_register = buttons_pressed();

    err = OSMboxPost(Mbox_Gas, button_register & GAS_PEDAL_FLAG? on : off);
    err = OSMboxPost(Mbox_Cruise,button_register & CRUISE_CONTROL_FLAG? on : off);
    err = OSMboxPost(Mbox_Brake, button_register & BRAKE_PEDAL_FLAG? on : off);

    int led_value=((button_register & CRUISE_CONTROL_FLAG)>0)*0xff &  LED_GREEN_2
                  | ((button_register & BRAKE_PEDAL_FLAG)>0)*0xff  & LED_GREEN_4
                  | ((button_register & GAS_PEDAL_FLAG)>0)*0xff  & LED_GREEN_6;
    change_led_button_status( 0x7E, led_value);

    OSSemPend(ButtonTaskTimerSem, 0, &err);
  }

}

/*
  * The task 'SwitchIOTask' read the data from the switches and manage the leds linked
  * to them
  */
void SwitchIOTask(void* pdata)
{
  INT8U err;
  int switch_register;
  printf("SwitchIO Task created!\n");

  while(1)
  {
    switch_register = (~switch_register) & 0xf;
    switch_register = switches_pressed();
    err = OSMboxPost(Mbox_Engine, switch_register & ENGINE_FLAG ? on : off);
    err = OSMboxPost(Mbox_Gear,switch_register & TOP_GEAR_FLAG ? on : off);

    int led_value=((switch_register & ENGINE_FLAG)>0)*0xff &  LED_RED_0
                  | ((switch_register & TOP_GEAR_FLAG)>0)*0xff  & LED_RED_1;

    change_led_switch_status(0x3,led_value);

    OSSemPend(SwitchTaskTimerSem, 0, &err);
  }

}


/*
  * The task 'OverloadDetectionTask' notify using a shared variable the watchdog
  * if there is an overload or not
  */
void OverloadDetectionTask(void* pdata)
{
  INT8U err;
  printf("Overload Task created!\n");

  while(1)
  {
    OK = 1;
    //printf("utilization is %d \n", utilization_value);
    OSSemPend(OverloadDetectionTaskTimerSem, 0, &err);
  }
}


/*
  * The task 'WatchdogTask' check if there are overload and in case it sends a warning message
  */
void WatchdogTask(void* pdata)
{
  INT8U err;
  printf("Watchdog Task created!\n");

  while(1)
  {
    if(OK == 0)
    {
      printf("**** WARNING : Overload detected! ****\n");
      err = OSMboxPost(Mbox_Reset, 1);
    }
    printf("OK IS %d\n",OK );
    OK = 0;
    printf("Watchdog working\n");
    OSSemPend(WatchdogTaskTimerSem, 0, &err);
  }
}


/*
  * The task 'ExtraLoadTask' is a dummy task to simualt extra work in the board
  */
void ExtraLoadTask(void* pdata)
{
  INT8U err;
  int switch_register;
  printf("ExtraLoad Task created!\n");
  INT32U mask= 0x3F0; // 000000001111110000

  long double micro_second_time;
  int clock_cycles;
  void* overload_message;

  while(1)
  {
    //Read the amount of extra load
    switch_register = (~switch_register) & 0xf;
    switch_register = switches_pressed();

    //Evaluation of amount of extra load
    int utilization_value = (switch_register & mask)>>4;
    utilization_value = 2*utilization_value;

    if(utilization_value> 100)
    {
      utilization_value = 100;
    }
    printf("utilization is %d \n", utilization_value);

    //Delay of a percentage of the control period to simulate an extra load
    PERF_RESET( PERFORMANCE_COUNTER_BASE );
    PERF_START_MEASURING( PERFORMANCE_COUNTER_BASE );
    do{
      clock_cycles= perf_get_total_time( (void *) PERFORMANCE_COUNTER_BASE );

      micro_second_time = (long double) clock_cycles/ 50 ;
      overload_message = OSMboxAccept(Mbox_Reset);
    }
   while(micro_second_time/1000<utilization_value/100*CONTROL_PERIOD && overload_message == (void *)0);
   PERF_STOP_MEASURING( PERFORMANCE_COUNTER_BASE );

   // OSTimeDlyHMSM(0,0,0,utilization_value/100*CONTROL_PERIOD);

    OSSemPend(ExtraLoadTaskTimerSem, 0, &err);
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
   * Create and start Software Timer
   */

   //Create Control Timer
   ControlTimer = OSTmrCreate(0, //delay
                            CONTROL_PERIOD/HW_TIMER_PERIOD, //period
                            OS_TMR_OPT_PERIODIC,
                            ControlTimerCallback, //OS_TMR_CALLBACK
                            (void *)0,
                            "ControlTimer",
                            &err);

   if (DEBUG) {
    if (err == OS_ERR_NONE) { //if creation successful
      printf("ControlTimer created\n");
    }
   }

   //Create Vehicle Timer
   VehicleTimer = OSTmrCreate(0, //delay
                            VEHICLE_PERIOD/HW_TIMER_PERIOD, //period
                            OS_TMR_OPT_PERIODIC,
                            VehicleTimerCallback, //OS_TMR_CALLBACK
                            (void *)0,
                            "VehicleTimer",
                            &err);

   if (DEBUG) {
    if (err == OS_ERR_NONE) { //if creation successful
      printf("VehicleTimer created\n");
    }
   }

   /*
    * Start timers
    */

   //start Vehicle Timer
   OSTmrStart(VehicleTimer, &err);

   if (DEBUG) {
    if (err == OS_ERR_NONE) { //if start successful
      printf("VehicleTimer started\n");
    }
   }

   //start Control Timer
   OSTmrStart(ControlTimer, &err);

   if (DEBUG) {
    if (err == OS_ERR_NONE) { //if start successful
      printf("ControlTimer started\n");
    }
   }

   /*
   * Creation of Kernel Objects
   */

  VehicleTimerSem = OSSemCreate(0);
  ControlTimerSem = OSSemCreate(0);
  SwitchTaskTimerSem = OSSemCreate(0);
  ButtonTaskTimerSem = OSSemCreate(0);
  OverloadDetectionTaskTimerSem = OSSemCreate(0);
  WatchdogTaskTimerSem = OSSemCreate(0);
  ExtraLoadTaskTimerSem = OSSemCreate(0);

  // Mailboxes
  Mbox_Throttle = OSMboxCreate((void*) 0); /* Empty Mailbox - Throttle */
  Mbox_Velocity = OSMboxCreate((void*) 0); /* Empty Mailbox - Velocity */
  Mbox_Brake = OSMboxCreate((void*) 0); /* Empty Mailbox - Brake*/
  Mbox_Gas = OSMboxCreate((void*) 0); /* Empty Mailbox -  Gas*/
  Mbox_Cruise = OSMboxCreate((void*) 0); /* Empty Mailbox - Cruise*/
  Mbox_Gear = OSMboxCreate((void*) 0); /* Empty Mailbox -  Gear*/
  Mbox_Engine = OSMboxCreate((void*) 0); /* Empty Mailbox - Engine */
  Mbox_Reset = OSMboxCreate((void*) 0); /* Empty Mailbox - Reset */



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
			&ButtonIOTask_Stack[TASK_STACKSIZE-1], // Pointer to top
			// of task stack
			BUTTONIOTASK_PRIO,
			BUTTONIOTASK_PRIO,
			(void *)&ButtonIOTask_Stack[0],
			TASK_STACKSIZE,
			(void *) 0,
			OS_TASK_OPT_STK_CHK);

  err = OSTaskCreateExt(
			SwitchIOTask, // Pointer to task code
			NULL,        // Pointer to argument that is
			// passed to task
			&SwitchIOTask_Stack[TASK_STACKSIZE-1], // Pointer to top
			// of task stack
			SWITCHIOTASK_PRIO,
			SWITCHIOTASK_PRIO,
			(void *)&SwitchIOTask_Stack[0],
			TASK_STACKSIZE,
			(void *) 0,
			OS_TASK_OPT_STK_CHK);

  err = OSTaskCreateExt(
			WatchdogTask, // Pointer to task code
			NULL,        // Pointer to argument that is
			// passed to task
			&WatchdogTask_Stack[TASK_STACKSIZE-1], // Pointer to top
			// of task stack
			WATCHDOGTASK_PRIO,
			WATCHDOGTASK_PRIO,
			(void *)&WatchdogTask_Stack[0],
			TASK_STACKSIZE,
			(void *) 0,
			OS_TASK_OPT_STK_CHK);

  err = OSTaskCreateExt(
     OverloadDetectionTask, // Pointer to task code
     NULL,        // Pointer to argument that is
     // passed to task
     &OverloadDetectionTask_Stack[TASK_STACKSIZE-1], // Pointer to top
     // of task stack
     OVERLOADDETECTIONTASK_PRIO,
     OVERLOADDETECTIONTASK_PRIO,
     (void *)&OverloadDetectionTask_Stack[0],
     TASK_STACKSIZE,
     (void *) 0,
     OS_TASK_OPT_STK_CHK);

   err = OSTaskCreateExt(
      ExtraLoadTask, // Pointer to task code
      NULL,        // Pointer to argument that is
      // passed to task
      &ExtraLoadTask_Stack[TASK_STACKSIZE-1], // Pointer to top
      // of task stack
      EXTRALOADTASK_PRIO,
      EXTRALOADTASK_PRIO,
      (void *)&ExtraLoadTask_Stack[0],
      TASK_STACKSIZE,
      (void *) 0,
      OS_TASK_OPT_STK_CHK);


  printf("All Tasks and Kernel Objects generated!\n");

  /* Task deletes itself */

  OSTaskDel(OS_PRIO_SELF);
}

/*static void Key_InterruptHandler(void* context, alt_u32 id)
{
  printf("Interrupt is called\n");
  /* Write to the edge capture register to reset it. */
  /*IOWR_ALTERA_AVALON_PIO_EDGE_CAP(DE2_PIO_KEYS4_BASE, 0);
  /* reset interrupt capability for the Button PIO. */
  /*IOWR_ALTERA_AVALON_PIO_IRQ_MASK(DE2_PIO_KEYS4_BASE, 0xf);
}*/

/*
 *
 * The function 'main' creates only a single task 'StartTask' and starts
 * the OS. All other tasks are started from the task 'StartTask'.
 *
 */


int main(void) {


  // set interrupt capability for the Button PIO.
 // IOWR_ALTERA_AVALON_PIO_IRQ_MASK(DE2_PIO_KEYS4_BASE, 0xf);
   // Reset the edge capture register.
  //IOWR_ALTERA_AVALON_PIO_EDGE_CAP(DE2_PIO_KEYS4_BASE, 0x0);
  // Register the ISR for buttons
  //alt_irq_register(DE2_PIO_KEYS4_IRQ, NULL, Key_InterruptHandler);


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
