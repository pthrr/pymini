/********************************************************************
* emc.h - Support definitions for rtstepperemc
*
*   Derived from a work by Fred Proctor & Will Shackleford
*
* License: GPL Version 2
*
* Copyright (c) 2004 All rights reserved.
*
* Re-purposed for rt-stepper dongle.
*
* Author: David Suffield, dsuffiel@ecklersoft.com
* (c) 2011-2017 Copyright Eckler Software
*
********************************************************************/
#ifndef _EMC_H
#define _EMC_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <pthread.h>
#include "emctool.h"
#include "tp.h"
#include "tc.h"

#if (defined(__WIN32__) || defined(_WINDOWS))
   #define DLL_EXPORT __declspec(dllexport)
#else
   #define DLL_EXPORT __attribute__ ((visibility("default")))
#endif

#define DUMMY_VARIABLE __attribute__((__unused__))

enum EMC_RESULT
{
   EMC_R_INVALID_GCODE_FILE = -11,
   EMC_R_INTERPRETER_ERROR = -10,
   RTSTEPPER_R_IO_TIMEDOUT = -9,
   RTSTEPPER_R_DEVICE_UNAVAILABLE = -8,
   RTSTEPPER_R_REQ_ERROR = -7,
   RTSTEPPER_R_MALLOC_ERROR = -6,
   RTSTEPPER_R_IO_ERROR = -5,
   EMC_R_INVALID_INI_KEY = -4,
   EMC_R_INVALID_INI_FILE = -3,
   EMC_R_TIMEOUT = -2,
   EMC_R_ERROR = -1,
   EMC_R_OK = 0,
   RTSTEPPER_R_INPUT_TRUE = 1,
   RTSTEPPER_R_INPUT_FALSE = 2,
   EMC_R_PROGRAM_PAUSED = 3,
   EMC_R_PROGRAM_END = 4,     /* potential use for M2 or M30 */
   RTSTEPPER_R_IO_CANCELED = 5, /* user cancel or estop */
};

#include "rtstepper.h"

enum EMC_AXIS_MAP
{
   EMC_AXIS_X = 0,
   EMC_AXIS_Y = 1,
   EMC_AXIS_Z = 2,
   EMC_AXIS_A = 3,
   EMC_AXIS_B = 4,
   EMC_AXIS_C = 5,
   EMC_AXIS_U = 6,
   EMC_AXIS_V = 7,
   EMC_AXIS_W = 8,
   EMC_MAX_AXIS = 9,
};

#define EMC_MAX_JOINTS EMC_MAX_AXIS

struct emc_axis
{
   int type;                    /* 0 = linear, 1 = rotary */
   double max_pos_limit;        /* upper soft position limit */
   double min_pos_limit;        /* lower soft position limit */
   double min_ferror;           /* zero speed following error limit */
   double max_ferror;           /* max speed following error limit */
   double home;                 /* joint coordinate of home point */
   double backlash;             /* amount of backlash */
   double max_acceleration;     /* upper limit of joint accel */
   double max_velocity;         /* upper limit of joint speed */

   /* Used in leadscrew compensation calculation. */
   double backlash_corr;        /* backlash correction */
   double backlash_filt;        /* filtered backlash correction */
   double backlash_vel;         /* backlash velocity variable */
   double pos_cmd;              /* trajectory planner commanded position */
   double vel_cmd;

   /* Set by .ini file. */
   int step_pin;                /* DB25 pin number */
   int direction_pin;           /* DB25 pin number */
   int step_active_high;       /* DB25 pin polarity */
   int direction_active_high;  /* DB25 pin polarity */
   double steps_per_unit;      /* INPUT_SCALE */
   int coordinate_map;         /* AXIS (step/dir pins) to coordinate map */

   /* Used in rtstpper_encode(). */
   int master_index;   /* running position in step counts */
   int clk_tail;       /* used calculate number cycles between pulses */
   int direction;      /* cycle time step direction */
};

/*
 * emc_session "state_bits" definition
 *
 * MSB                                                          LSB
 * +---------------------------------------v---------------------------------------+
 * | 31 | 30 | 29 | 28 | 27 | 26 | 25 | 24 | 23 | 22 | 21 | 20 | 19 | 18 | 17 | 16 |
 * ----------------------------------------^---------------------------------------+
 * |    |    |    |    |    |    |    |    |    |    |    |    |CANC|HOME|PAUS|ESTP|
 * +---------------------------------------v---------------------------------------+
 * | 15 | 14 | 13 | 12 | 11 | 10 |  9 |  8 |  7 |  6 |  5 |  4 |  3 |  2 |  1 |  0 |
 * ----------------------------------------^---------------------------------------+
 * |    |    |    |    |    |    |    |    |    | IN3| IN2| IN1| IN0|SYNC|EMPT|ABRT|
 * +---------------------------------------v---------------------------------------+
 *
 * where:
 *   ESTP = ESTOP (1=True, 0=False)
 *   PAUS = PROGRAM_PAUSED (1=True, 0=False)
 *   HOME = HOMED (1=True, 0=False)
 *   CANC = CANCEL AUTO/VERIFY (1=True, 0=False)
 *   ABRT = RTSTEPPER_ABORT (1=True, 0=False)
 *   EMPT = RTSTEPPER_EMPTY (1=True, 0=False)
 *   SYNC = RTSTEPPER_SYNC_START (1=True, 0=False)
 *   IN0  = RTSTEPPER_INPUT0 (1=True, 0=False)
 *   IN1  = RTSTEPPER_INPUT1 (1=True, 0=False)
 *   IN2  = RTSTEPPER_INPUT2 (1=True, 0=False)
 *   IN3  = RTSTEPPER_INPUT3 (1=True, 0=False)
 */

#define EMC_STATE_ESTOP_BIT 0x010000
#define EMC_STATE_PAUSED_BIT 0x020000
#define EMC_STATE_HOMED_BIT 0x040000
#define EMC_STATE_CANCEL_BIT 0x080000

/* size of motion queue, a TC_STRUCT is about 512 bytes so this queue is about a megabyte.  */
#define DEFAULT_TC_QUEUE_SIZE 2000

struct emc_session
{
   char ini_file[LINELEN];
   uint32_t state_bits;
   uint32_t old_state_bits;

   /* interpreter */
   FILE *gfile;                    /* gcode file */
   int line_number;                /* saved during program pause */

   /* trajectory planner */
   double cycle_time;              /* waypoint period in seconds */
   double cycle_freq;              /* 1 / cycle_time */
   TP_STRUCT tp_queue;             /* trajectory planner based on TC elements */
   TC_STRUCT tc_queue[DEFAULT_TC_QUEUE_SIZE + 10]; /* discriminate-based trajectory planning */

   /* rtstepper dongle */
   int req_cnt;                 /* number of queued usb io requests */
   struct rtstepper_io_req head;  /* usb step/dir queue */
   struct rtstepper_file_descriptor fd_table;
   char serial_num[64];         /* dongle usb serial number */
   int input0_abort_enabled;    /* 0=false, 1=true */
   int input1_abort_enabled;    /* 0=false, 1=true */
   int input2_abort_enabled;    /* 0=false, 1=true */
   int input3_abort_enabled;    /* 0=false, 1=true */
   uint16_t icount_period;      /* input0 frequency period in counts */
   uint16_t icount_period_avg;      /* input0 frequency period average */
   uint16_t icount_period_max;
   uint16_t icount_period_min;
   int step_clock;         /* step clock frequency */
   struct rtstepper_moving_avg icount_ma;
   enum RTSTEPPER_IO_TYPE current_io_type;
   struct rtstepper_ctrl_req ctrl_head;  /* usb control cmd queue */
   uint8_t input1_adc;  /* ADC 8-bit value */
   uint8_t input2_adc;
   uint8_t input3_adc;


   /* task */
   int programUnits;            // CANON_UNITS_INCHES,MM,CM

   /* motion */
   double linearUnits;          // units per mm (ini: TRAJ, LINEAR_UNITS)
   double angularUnits;         // units per degree (ini: TRAJ, ANGULAR_UNITS)
   unsigned int axes;           // maximum axis number (ini: TRAJ, AXES)
   unsigned int axes_mask;      // mask of axes actually present
   EmcPose position;            // current commanded position
   double maxVelocity;          // max system velocity
   double maxAcceleration;      // max system acceleration
   double sync_feed_per_sec;    /* feed_per_second used for syncronized motion */
   int sync_enabled;            /* syncronized motion, 0=false, 1=true) */

   /* io */
   struct CANON_TOOL_TABLE toolTable[CANON_POCKETS_MAX];

   /* axis */
   struct emc_axis axis[EMC_MAX_AXIS];
};

enum EMC_TASK_STATE
{
   EMC_TASK_STATE_UNUSED = 0,
   EMC_TASK_STATE_ESTOP = 1,
   EMC_TASK_STATE_ESTOP_RESET = 2,
   EMC_TASK_STATE_OFF = 3,
   EMC_TASK_STATE_ON = 4
};

/* Localization helper. */
#ifndef EMC_I18N
#define EMC_I18N(text) text
#endif

#define container_of(ptr, type, member) ({            \
 const typeof( ((type *)0)->member ) *__mptr = (ptr);    \
 (type *)( (char *)__mptr - offsetof(type,member) );})

#define DEFAULT_RS274NGC_STARTUP_CODE ""

/* default interp len */
#define DEFAULT_EMC_TASK_INTERP_MAX_LEN 1000

/* default name of EMC_TOOL tool table file */
#define DEFAULT_TOOL_TABLE_FILE "tool.tbl"

/* default feed rate, in user units per second */
#define DEFAULT_TRAJ_DEFAULT_VELOCITY 1.0

/* default traverse rate, in user units per second */
#define DEFAULT_TRAJ_MAX_VELOCITY 10.0

/* default axis traverse rate, in user units per second */
#define DEFAULT_AXIS_MAX_VELOCITY 1.0

/* default axis acceleration, in user units per second per second */
#define DEFAULT_AXIS_MAX_ACCELERATION 1.0

/* Values for EMC_AXIS_SET_AXIS, axisType. */
enum EmcAxisType
{
   EMC_AXIS_LINEAR = 1,
   EMC_AXIS_ANGULAR = 2,
};

/* Set the units conversion factor. @see EMC_AXIS_SET_INPUT_SCALE  */
typedef double EmcLinearUnits;
typedef double EmcAngularUnits;

enum EMC_MOTION_TYPE
{
   EMC_MOTION_TYPE_TRAVERSE = 1,   /* G0 */
   EMC_MOTION_TYPE_FEED,           /* G1 */
   EMC_MOTION_TYPE_ARC,
   EMC_MOTION_TYPE_TOOLCHANGE,
   EMC_MOTION_TYPE_PROBING,
};

enum EMC_COMMAND_MSG_TYPE
{
   EMC_COMMAND_UNUSED = 0,
   EMC_TRAJ_LINEAR_MOVE_TYPE,
   EMC_TRAJ_SET_TERM_COND_TYPE,
   EMC_TRAJ_CIRCULAR_MOVE_TYPE,
   EMC_TRAJ_DELAY_TYPE,
   EMC_SYSTEM_CMD_TYPE,
   EMC_TASK_PLAN_PAUSE_TYPE,
   EMC_TASK_PLAN_END_TYPE,
   EMC_START_SPEED_FEED_SYNCH,
   EMC_STOP_SPEED_FEED_SYNCH,
};

/* message header */
typedef struct _emc_msg_t
{
   enum EMC_COMMAND_MSG_TYPE type;
   unsigned int n;              /* sequence number */
//   struct list_head list;
} emc_msg_t;

typedef struct _emc_traj_set_origin_msg_t
{
   emc_msg_t msg;
   EmcPose origin;
} emc_traj_set_origin_msg_t;

typedef struct _emc_traj_set_rotation_msg_t
{
   emc_msg_t msg;
   double rotation;
} emc_traj_set_rotation_msg_t;

typedef struct _emc_traj_linear_move_msg_t
{
   emc_msg_t msg;
   enum EMC_MOTION_TYPE type;
   EmcPose end;                 // end point
   double vel, ini_maxvel, acc;
   int feed_mode;
} emc_traj_linear_move_msg_t;

typedef struct _emc_traj_set_term_cond_msg_t
{
   emc_msg_t msg;
   int cond; 
   double tolerance; 
} emc_traj_set_term_cond_msg_t;

typedef struct _emc_traj_circular_move_msg_t
{
   emc_msg_t msg;
   EmcPose end;
   PmCartesian center;
   PmCartesian normal;
   int turn;
   int type;
   double vel, ini_maxvel, acc;
   int feed_mode;
} emc_traj_circular_move_msg_t;

typedef struct _emc_traj_delay_msg_t
{
   emc_msg_t msg;
   double delay;                // seconds
} emc_traj_delay_msg_t;

typedef struct _emc_system_cmd_msg_t
{
   emc_msg_t msg;
   int index;          /* user defined mcode m100-m199 */
   double p_number;
   double q_number;
} emc_system_cmd_msg_t;

typedef struct _emc_start_speed_feed_synch_msg_t
{
   emc_msg_t msg;
   double feed_per_revolution;  /* distance per revolution (pitch) */
   int velocity_mode;
} emc_start_speed_feed_synch_msg_t;

/* generic command message */
typedef struct _emc_command_msg_t
{
   union
   {
      emc_msg_t msg;
      emc_traj_set_origin_msg_t m1;
      emc_traj_set_rotation_msg_t m2;
      emc_traj_linear_move_msg_t m3;
      emc_traj_set_term_cond_msg_t m4;
      emc_traj_circular_move_msg_t m5;
      emc_traj_delay_msg_t m6;
      emc_system_cmd_msg_t m7;
   };
} emc_command_msg_t;

struct post_position_py;
typedef void *(*logger_cb_t) (const char *msg);
typedef void *(*post_position_cb_t) (int cmd, struct post_position_py *pospy);
typedef void *(*post_event_cb_t) (int cmd);
typedef int (*plugin_cb_t) (int mcode, double p_number, double q_number);

extern char USER_HOME_DIR[];
extern struct emc_session session;

/* Forward declarations. */
struct emcpose_py;

#ifdef __cplusplus
extern "C"
{
#endif
   void esleep(double seconds);
   enum EMC_RESULT emc_logger_cb(const char *fmt, ...);
   enum EMC_RESULT emc_plugin_cb(int mcode, double p_number, double q_number);
   enum EMC_RESULT emc_position_post_cb(int id, EmcPose pos);
   enum EMC_RESULT emc_estop_post_cb(struct emc_session *ps);
   enum EMC_RESULT emc_paused_post_cb(struct emc_session *ps);
   DLL_EXPORT void *emc_ui_open(const char *home, const char *ini_file);
   DLL_EXPORT enum EMC_RESULT emc_ui_close(void *hd);
   DLL_EXPORT enum EMC_RESULT emc_ui_estop(void *hd);
   DLL_EXPORT enum EMC_RESULT emc_ui_estop_reset(void *hd);
   DLL_EXPORT enum EMC_RESULT emc_ui_logger_register_cb(logger_cb_t fp);
   DLL_EXPORT enum EMC_RESULT emc_ui_gui_event_register_cb(post_event_cb_t fp);
   DLL_EXPORT enum EMC_RESULT emc_ui_position_register_cb(post_position_cb_t fp);
   DLL_EXPORT enum EMC_RESULT emc_ui_plugin_register_cb(plugin_cb_t fp);
   DLL_EXPORT enum EMC_RESULT emc_ui_io_done_wait(void *hd);
   DLL_EXPORT enum EMC_RESULT emc_ui_home(void *hd);
   DLL_EXPORT enum EMC_RESULT emc_ui_version_get(const char **ver);
   DLL_EXPORT enum EMC_RESULT emc_ui_position_get(void *hd, struct emcpose_py *pospy);
   DLL_EXPORT enum EMC_RESULT emc_ui_position_set(void *hd, struct emcpose_py *pospy);
   DLL_EXPORT enum EMC_RESULT emc_ui_mdi_cmd(void *hd, const char *mdi);
   DLL_EXPORT enum EMC_RESULT emc_ui_auto_cmd(void *hd, const char *gcode_file);
   DLL_EXPORT enum EMC_RESULT emc_ui_auto_cancel_set(void *hd);
   DLL_EXPORT enum EMC_RESULT emc_ui_auto_cancel_clear(void *hd);
   DLL_EXPORT enum EMC_RESULT emc_ui_din_abort_enable(void *hd, int input_num);
   DLL_EXPORT enum EMC_RESULT emc_ui_din_abort_disable(void *hd, int input_num);
   DLL_EXPORT enum EMC_RESULT emc_ui_verify_cmd(void *hd, const char *gcode_file);
   DLL_EXPORT enum EMC_RESULT emc_ui_verify_cancel_set(void *hd);
   DLL_EXPORT enum EMC_RESULT emc_ui_verify_cancel_clear(void *hd);
   DLL_EXPORT enum EMC_RESULT emc_ui_test(const char *snum);
   DLL_EXPORT enum EMC_RESULT emc_ui_dout_set(void *hd, int output_num);
   DLL_EXPORT enum EMC_RESULT emc_ui_dout_clear(void *hd, int output_num);
   DLL_EXPORT enum EMC_RESULT emc_ui_din_frequency_get(void *hd, int input_num, double *value);
   DLL_EXPORT enum EMC_RESULT emc_ui_din_frequency_avg_get(void *hd, int input_num, double *value);
   DLL_EXPORT enum EMC_RESULT emc_ui_din_frequency_max_get(void *hd, int input_num, double *value);
   DLL_EXPORT enum EMC_RESULT emc_ui_din_frequency_min_get(void *hd, int input_num, double *value);
   enum EMC_RESULT dsp_open(struct emc_session *ps);
   enum EMC_RESULT dsp_close(struct emc_session *ps);
   enum EMC_RESULT dsp_mdi(struct emc_session *ps, const char *mdi);
   enum EMC_RESULT dsp_auto(struct emc_session *ps, const char *gcodefile);
   enum EMC_RESULT dsp_auto_cancel_set(struct emc_session *ps);
   enum EMC_RESULT dsp_auto_cancel_clear(struct emc_session *ps);
   enum EMC_RESULT dsp_estop(struct emc_session *ps);
   enum EMC_RESULT dsp_estop_reset(struct emc_session *ps);
   enum EMC_RESULT dsp_io_done_wait(struct emc_session *ps);
   enum EMC_RESULT dsp_home(struct emc_session *ps);
   enum EMC_RESULT dsp_din_abort_enable(struct emc_session *ps, int num);
   enum EMC_RESULT dsp_din_abort_disable(struct emc_session *ps, int num);
   enum EMC_RESULT dsp_verify(struct emc_session *ps, const char *gcodefile);
   enum EMC_RESULT dsp_verify_cancel_set(struct emc_session *ps);
   enum EMC_RESULT dsp_verify_cancel_clear(struct emc_session *ps);
   enum EMC_RESULT dsp_position_set(struct emc_session *ps, EmcPose pos);
   enum EMC_RESULT dsp_dout_set(struct emc_session *ps, int output_num);
   enum EMC_RESULT dsp_dout_clear(struct emc_session *ps, int output_num);
   enum EMC_RESULT dsp_paused_clear(struct emc_session *ps);
   enum EMC_RESULT dsp_output_mode(struct emc_session *ps, int output_num, int param);
   enum EMC_RESULT dsp_output_pwm(struct emc_session *ps, int output_num, int param);
   enum EMC_RESULT dsp_input_mode(struct emc_session *ps, int input_num, int param);
   const char *lookup_task_interp_state(int type);
   const char *lookup_message(int type);
   void compute_screw_comp(struct emc_session *ps);
   void reset_screw_comp(struct emc_session *ps);
   void update_tp_position(struct emc_session *ps, EmcPose pos);

#ifdef __cplusplus
}                               /* matches extern "C" at top */
#endif

#endif                          // _EMC_H
