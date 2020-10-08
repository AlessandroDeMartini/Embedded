// Final document Cruise Control

#include <stdio.h>
#include "system.h"
#include "includes.h"
#include "altera_avalon_pio_regs.h"
#include "sys/alt_irq.h"
#include "sys/alt_alarm.h"
#include "sys/alt_timestamp.h"
#include "sys/alt_cache.h"

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

OS_STK Watchdog_Stack[TASK_STACKSIZE];
OS_STK Overload_Stack[TASK_STACKSIZE];
OS_STK Extraload_Stack[TASK_STACKSIZE];

// Task Priorities

#define STARTTASK_PRIO      2
#define WATCHDOGTASK_PRIO   4    //high priority low number
#define VEHICLETASK_PRIO    6
#define CONTROLTASK_PRIO   12

#define BUTTONIOTASK_PRIO   8
#define SWITCHIOTASK_PRIO   7

#define OVERLOADTASK_PRIO  10    //low priority high number
#define EXTRALOADTASK_PRIO 16    //low priority high number

// Task Periods

#define CONTROL_PERIOD   300
#define VEHICLE_PERIOD   300
#define OVERLOAD_PERIOD  300

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

OS_EVENT *Mbox_Reset;

OS_EVENT *Mbox_Engine;
OS_EVENT *Mbox_Gear;
OS_EVENT *Mbox_Gas;
//OS_EVENT *Mbox_Reset;

// Semaphores

OS_EVENT *VehicleTmrSem;
OS_EVENT *ControlTmrSem;
OS_EVENT *ButtonTmrSem;
OS_EVENT *SwitchTmrSem;

OS_EVENT *OverloadDetectionTaskTimerSem;
OS_EVENT *WatchdogTaskTimerSem;
OS_EVENT *ExtraLoadTaskTimerSem;

// SW-Timer

OS_TMR *VehicleTmr;
OS_TMR *ControlTmr; // Since they have the same period the callback function could be just one (Possibility to add two other functions)
OS_TMR *OverloadTmr;

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

int overload_signal = 0; //signal send by the overload
//int check_signal = 0;     //signal send by the overload

int buttons_pressed(void)
{
  return ~IORD_ALTERA_AVALON_PIO_DATA(D2_PIO_KEYS4_BASE);    
}

int switches_pressed(void)
{
  return IORD_ALTERA_AVALON_PIO_DATA(DE2_PIO_TOGGLES18_BASE);    
}

/*
  * Definition of two functions to light up the LEDs we need. Red has 18 bit and green has 9.
  * We use a mask in order to change just the bit we need. Moreover, we reset them each time
  * The switch is move-in off position
*/

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
  if (DEBUG) 
    printf("OSSemPost(VehicleTmr);\n");
}

void ControlTmrCallback (void *ptmr, void *callback_arg)
{
  OSSemPost(ControlTmrSem);
  OSSemPost(ButtonTmrSem);  // Same period, we don't need others timers 
  OSSemPost(SwitchTmrSem);  // Same period, we don't need others timers
  if (DEBUG) 
    printf("OSSemPost(ControlTmr);\n");
}

void resetOverloadCallback  (void* ptmr, void* callback_arg)
{
  INT8U err;
  // if(check_signal == 1)
  //      check_signal = 0;
  // else
  //      overload_signal = 2;

  OSSemPost(WatchdogTaskTimerSem);

  // Reset if there was no Pend in the Past HyperPeriod
  OSSemSet(OverloadDetectionTaskTimerSem,0,&err); 
  OSSemPost(OverloadDetectionTaskTimerSem);
  
  // Reset if there was no Pend in the Past HyperPeriod
  OSSemSet(ExtraLoadTaskTimerSem,0,&err);
  OSSemPost(ExtraLoadTaskTimerSem);

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
 * output current velocity on the seven segment display
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
 */

void show_position(INT16U position)
{

  INT32U led_interested = 0x3F000; //mask --> which are the LEDS I'm interesting in
  INT32U led_ON = 0;
  if(position<400)
    led_ON = LED_RED_17;
  else if(position<800)
    led_ON = LED_RED_16;
  else if(position<1200)
    led_ON = LED_RED_15;
  else if(position<1600)
    led_ON = LED_RED_14;
  else if(position<2000)
    led_ON = LED_RED_13;
  else if(position<=2400)
    led_ON = LED_RED_12;

  change_RED_led_status(led_interested, led_ON);

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
  // enum active brake_pedal = off; // It created a conflict with the global variable

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
      // velocity = velocity  + acceleration * VEHICLE_PERIOD / 1000.0;
      
      // if(position > 2400)
      //   position = 0;

      position = adjust_position(position, velocity, acceleration, VEHICLE_PERIOD);
      velocity = adjust_velocity(velocity, acceleration, brake_pedal, VEHICLE_PERIOD);

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
  *cruise_velocity  = 0;
  *current_velocity = 0;

  // Store imputs from the swetches;
  int gas_pedal_tmp=0;
  int top_gear_tmp=0;
  int engine_tmp=0;
  int cruise_control_tmp=0;

  printf("Control Task created!\n");

  while(1)
    {
      msg = OSMboxPend(Mbox_Velocity, 1, &err);
      if (err == OS_NO_ERR)
         current_velocity = (INT16S*) msg;
       
      msg = OSMboxPend(Mbox_Cruise, 1, &err);
      if (err == OS_NO_ERR)
        cruise_velocity = (INT16S*) msg;
      
      if (DEBUG) 
      {
        printf("CRUISE VELOCITY: %d \n", *cruise_velocity);
        printf("CURRENT VELOCITY: %d \n", *current_velocity);
      }

      // Use green led to indicate cruise is on
      change_GREEN_led_status(0x1, (cruise_control == on)*0xff & LED_GREEN_0);

      // Se ho il cruise attivo prendo la velocità desiderata che rimane fissa a +- 4m/s
      
      
      if(*current_velocity < 25 || cruise_control == off)
      {
        cruise_control == off;
        show_target_velocity(0);
        
        if ( *current_velocity != 0 )
        {
          engine = on;     
          //change_RED_led_status(led_interested, LED_RED_0);
        }
      }

      if (cruise_control == on)
      {
        show_target_velocity(*cruise_velocity);
        
        // Basic proportional control
        if( (*cruise_velocity - *current_velocity) > 2 )
          throttle = throttle + 10;
        if( (*cruise_velocity - *current_velocity) < 2 )
          throttle = throttle - 15;
      }

      // acceleratio -> increase power
      else if(gas_pedal == on)
        throttle = 80;
      // when the engine is on the car have a certain power
      else if(engine == on){ 
        throttle = 40; 
      }

      err = OSMboxPost(Mbox_Throttle, (void *) &throttle);
      
      OSSemPend(ControlTmrSem, 0, &err);
    }
}


 /* The task ButtonIOTask permits to receive inputs from buttons
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

    switch (ButtonState)
      {
        case CRUISE_CONTROL_FLAG:   // Key1 is pressed

          if(top_gear == on && *current_velocity >= 25)
          {
            if (DEBUG) 
              printf( "CRUISE_CONTROL_FLAG \n");

            cruise_control = on;    // start cruise control 
            int cruise_velocity = *current_velocity;

            // Send the cruise velocity to the control task
            err = OSMboxPost(Mbox_Cruise, (void *) &cruise_velocity);

            change_GREEN_led_status(0x7E, LED_GREEN_2);
          }

        break;
        case BRAKE_PEDAL_FLAG:      // Key2 is pressed
            if (DEBUG)
              printf( "BRAKE_PEDAL_FLAG \n");

            brake_pedal = on;       // start brake    
            cruise_control = off;   // cruise off   

            change_GREEN_led_status(0x7E, LED_GREEN_4);
        break;
        case GAS_PEDAL_FLAG:        // Key3 is pressed
            if (DEBUG)
              printf( "GAS_PEDAL_FLAG \n");

            gas_pedal = on;               // start gas      
            cruise_control = off;   // cruise off
            
            change_GREEN_led_status(0x7E, LED_GREEN_6);
        break;
        default:

          gas_pedal   = off;
          brake_pedal = off;
          if(cruise_control == on)
            change_GREEN_led_status(0x7E, LED_GREEN_0);
          else
            change_GREEN_led_status(0x7E, NULL);
          
          if (DEBUG)
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
  INT32U led_interested = 0x3;
  printf ("SwitchIO Task created!\n");
  
  while (1)
   {
      SwitchState = (~SwitchState) & 0xf;
      SwitchState = switches_pressed(); // 1,2,3 considering how many switches are on

      switch (SwitchState)
      {
        case ENGINE_FLAG:                // Switch0 is pressed
          if (DEBUG)
            printf ("ENGINE_FLAG \n");
          
          engine = on;                   // engine on 
          cruise_control = off;

          change_RED_led_status(led_interested, LED_RED_0);
          
        break;
        case TOP_GEAR_FLAG:             // Switch1 is pressed
          if (DEBUG)
            printf ("TOP_GEAR_FLAG \n");
          
          top_gear = on;      
          
          change_RED_led_status(led_interested, LED_RED_1);
  
        break;
        case TOP_GEAR_FLAG+ENGINE_FLAG :             // Switch1 is pressed

          if (DEBUG)
            printf ("TOP_GEAR_FLAG + ENGINE_FLAG \n");
          
          top_gear = on;
          engine = on;         
          
          change_RED_led_status(led_interested, LED_RED_1 + LED_RED_0);
 
        break;
        default:
          
          engine = off; // engine problem

          top_gear = off;
          cruise_control = off;

          change_RED_led_status(led_interested, NULL);
          
          if (DEBUG)
            printf("Default state: engine, top_gear off \n");

        break;
      }
     OSSemPend(SwitchTmrSem, 0, &err);
   }
}

void OverloadTask(void *pdata)     
{
    INT8U err;
    printf("Overload created \n");  //Debug print

    while(1)
    {
      //if (check_signal == 0)
      //{
      overload_signal = 1; 
      //}
      OSSemPend(OverloadDetectionTaskTimerSem, 0, &err);
    }
}

void WatchdogTask(void *pdata)     
{
    INT8U err;
    printf("Watchdog created \n");

    while(1)
    {
      if(overload_signal == 0)
      {
        printf("WARNING! Overload detected \n");
        err = OSMboxPost(Mbox_Reset, 1);
      }
      overload_signal = 0;
      printf("Watchdog working\n");

      OSSemPend(WatchdogTaskTimerSem, 0, &err);
    }
}

void ExtraloadTask(void *pdata)     
{
  INT8U err;
  int SwitchState;
  printf("ExtraloadTask created \n");

  INT32U led_interested = 0x3F0;  // 000000001111110000

  long double micro_sec_time;
  int clock_cycles;
  void* overload_message;

  while(1)
  {
    SwitchState = (~SwitchState) & 0xf;
    SwitchState = switches_pressed();

    // Compute the value of the utilization
    int utilization_value = (SwitchState & led_interested) >> 4;
    utilization_value = 2*utilization_value;
    
    if(utilization_value> 100)
      utilization_value = 100;

    if(DEBUG)
      printf("utilization is %d \n", utilization_value);

    //Delay of a percentage of the control period to simulate an extra load
    PERF_RESET( PERFORMANCE_COUNTER_BASE );
    PERF_START_MEASURING( PERFORMANCE_COUNTER_BASE );

    do{
      clock_cycles= perf_get_total_time( (void *) PERFORMANCE_COUNTER_BASE );

      micro_sec_time = (long double) clock_cycles/ 50 ;
      overload_message = OSMboxAccept(Mbox_Reset);
    }
    while (micro_sec_time/1000 < utilization_value/100*CONTROL_PERIOD && overload_message == (void *)0 );
    
    PERF_STOP_MEASURING( PERFORMANCE_COUNTER_BASE );

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


   OverloadTmr = OSTmrCreate (0,                          // delay
                              OVERLOAD_PERIOD/HW_TIMER_PERIOD,       // period
                              OS_TMR_OPT_PERIODIC,        
                              resetOverloadCallback ,              // OS_TMR_CALLBACK
                              (void *)0,                  
                              "OverloadTmr",                       
                              &err);
                        
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

  //start  OverloadTmr
  OSTmrStart(OverloadTmr, &err);
   
  if (DEBUG) 
  {
    if (err == OS_ERR_NONE) 
    { //if start successful
      printf("OverloadTmr started\n");
    }
  } 


  /*
   * Creation of Kernel Objects
   */
  
  VehicleTmrSem = OSSemCreate(0);   
  ControlTmrSem = OSSemCreate(0); 
  ButtonTmrSem  = OSSemCreate(0);
  SwitchTmrSem  = OSSemCreate(0);
  OverloadDetectionTaskTimerSem = OSSemCreate(0);
  WatchdogTaskTimerSem = OSSemCreate(0);
  ExtraLoadTaskTimerSem = OSSemCreate(0);

  // Mailboxes
  Mbox_Throttle = OSMboxCreate((void*) 0); /* Empty Mailbox - Throttle */
  Mbox_Velocity = OSMboxCreate((void*) 0); /* Empty Mailbox - Velocity */
  Mbox_Brake    = OSMboxCreate((void*) 0); /* Empty Mailbox - Brake */
  Mbox_Cruise   = OSMboxCreate((void*) 0); /* Empty Mailbox - Cruise */
  Mbox_Gas      = OSMboxCreate((void*) 0);
  Mbox_Gear     = OSMboxCreate((void*) 0); /* Empty Mailbox -  Gear*/
  Mbox_Engine   = OSMboxCreate((void*) 0); 
  Mbox_Reset    = OSMboxCreate((void*) 0); 
  Mbox_Velocity_BUTTON = OSMboxCreate((void*) 0);
  
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
  
  err = OSTaskCreateExt(
      OverloadTask, // Pointer to task code
      NULL,        // Pointer to argument that is
      // passed to task
      &Overload_Stack[TASK_STACKSIZE-1], // Pointer to top
      // of task stack
      OVERLOADTASK_PRIO,
      OVERLOADTASK_PRIO,
      (void *)&Overload_Stack[0],
      TASK_STACKSIZE,
      (void *) 0,
      OS_TASK_OPT_STK_CHK);

  err = OSTaskCreateExt(
      WatchdogTask, // Pointer to task code
      NULL,        // Pointer to argument that is
      // passed to task
      &Watchdog_Stack[TASK_STACKSIZE-1], // Pointer to top
      // of task stack
      WATCHDOGTASK_PRIO,
      WATCHDOGTASK_PRIO,
      (void *)&Watchdog_Stack[0],
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