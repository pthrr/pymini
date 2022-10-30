/************************************************************************************\

  rtstepper.h - rt-stepper dongle support for EMC2

  (c) 2011-2015 Copyright Eckler Software

  Author: David Suffield, dsuffiel@ecklersoft.com

  This program is free software; you can redistribute it and/or modify
  it under the terms of version 2 of the GNU General Public License as published by
  the Free Software Foundation.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA

  Upstream patches are welcome. Any patches submitted to the author must be 
  unencumbered (ie: no Copyright or License).

  See project revision history the "configure.ac" file.

\************************************************************************************/

#ifndef _RTSTEPPER_H
#define _RTSTEPPER_H

#include <stdint.h>
#include <pthread.h>
#include <libusb.h>
#include "list.h"
#include "emc.h"

enum INPUT_PIN_NUM { INPUT0_NUM, INPUT1_NUM, INPUT2_NUM, INPUT3_NUM };
enum OUTPUT_PIN_NUM { OUTPUT0_NUM, OUTPUT1_NUM, OUTPUT2_NUM };

/* EP0 Vendor Setup commands (bRequest). */
enum STEP_CMD
{
   STEP_SET,                    /* set step elements, clear state bits */
   STEP_QUERY,                  /* query current step and state info */
   STEP_ABORT_SET,              /* set un-synchronized stop */
   STEP_ABORT_CLEAR,            /* clear un-synchronized stop */
   STEP_OUTPUT0_SET,    /* digital only */
   STEP_OUTPUT0_CLEAR,  /* (5) digital only */
   STEP_OUTPUT1_SET,  /* digital only */
   STEP_OUTPUT1_CLEAR,  /* digital only */
   STEP_SYNC_START_SET,  /* set synchronized start */
   /* Following are new for FW REV-3f dongle. */
   STEP_OUTPUT2_SET,     /* digital only */
   STEP_OUTPUT2_CLEAR,   /* (10, 0xA) digital only */
   STEP_OUTPUT0_MODE,   /* digital | pwm */
   STEP_OUTPUT0_PWM,    /* 0-255 duty cycle inverted */
   STEP_OUTPUT1_MODE,   /* (13, 0xD) digital | pwm */
   STEP_OUTPUT1_PWM,    /* 0-255 duty cycle non-inverted */
   STEP_INPUT1_MODE,    /* (15, 0xF) digital | adc */
   STEP_INPUT2_MODE,    /* digital | adc */
   STEP_INPUT3_MODE,    /* digital | adc */
   STEP_ADC_QUERY,     /* (18, 0x12) query state and adc info */
   STEP_CMD_MAX,
};

/* OUTPUT0_MODE and OUTPUT1_MODE command parameters (wValue). */
enum OUTPUT_MODE_PARAM
{
   OMP_DIGITAL,
   OMP_PWM,
   OMP_MAX,
};

/* INPUT1_MODE, INPUT2_MODE and INPUT3_MODE command parameters (wValue). */
enum INPUT_MODE_PARAM
{
   IMP_DIGITAL,
   IMP_ADC,
   IMP_MAX,
};

enum RTSTEPPER_IO_TYPE
{
   RTSTEPPER_IO_TYPE_SPINDLE_ASYNC,  /* feed is NOT synchronized to spindle */
   RTSTEPPER_IO_TYPE_SPINDLE_SYNC,   /* feed is synchronized to spindle */
};

struct rtstepper_io_req
{
   int id;
   EmcPose position;            // commanded position
   unsigned char *buf;          /* step/direction buffer */
   int buf_size;                /* buffer size in bytes */
   int total;                   /* current buffer count, number of bytes used (total < buf_size) */
   enum RTSTEPPER_IO_TYPE type;
   struct emc_session *session;
   struct libusb_transfer *req; 
   struct list_head list;
};

struct rtstepper_ctrl_req
{
   enum STEP_CMD cmd;
   uint16_t param;
   struct list_head list;
};

/*
 * Dongle board FW releases. 1x = PIC18f2455, 2x = PIC16f1459, 3x = ATmega32U4
 * PIC18f2455 max support = 1e, PIC16f1459 max support = 2e
 */
enum RTSTEPPER_BRD
{
   RTSTEPPER_BRD_NA,
   RTSTEPPER_BRD_a,    /* 1a new */
   RTSTEPPER_BRD_b,    /* 1b Microsoft OS descriptors */
   RTSTEPPER_BRD_c,    /* 1c OUTPUT0-1, usb suspend */
   RTSTEPPER_BRD_d,    /* 1d INPUT0 frequency counter */
   RTSTEPPER_BRD_e,    /* 1e, 2e index pulse threading */
   RTSTEPPER_BRD_f,    /* 3f AVR ATmega32U4 */
   RTSTEPPER_BRD_NEW,
};

struct rtstepper_file_descriptor
{
   libusb_device_handle *hd;
   libusb_device **list_all;
   libusb_context *ctx;
   libusb_device *dev;
   int event_done;
   int event_abort_done;
   pthread_t event_tid;    /* thread handle */
   int dongle_done;
   int dongle_abort_done;
   pthread_t dongle_tid;    /* thread handle */
   enum RTSTEPPER_BRD board_rev;
};

struct rtstepper_moving_avg
{
   int sum;
   int array[4];
   int pos;      /* pointer to array[] */
};

#define RTSTEPPER_ICOUNT_MAX 0xffff      /* max value for icount_period */
#define RTSTEPPER_STEP_CLOCK_PIC 46875       /* PIC18F2455/PIC16F1459 */
#define RTSTEPPER_STEP_CLOCK_AVR 62500   /* AVR ATmega32U4 */

#define RTSTEPPER_STEP_STATE_ABORT_BIT 0x01     /* abort step buffer, 1=True, 0=False (R/W) */
#define RTSTEPPER_STEP_STATE_EMPTY_BIT 0x02     /* step buffer empty, 1=True, 0=False */
#define RTSTEPPER_STEP_STATE_SYNC_START_BIT 0x04     /* 1=True, 0=False */
#define RTSTEPPER_STEP_STATE_INPUT0_BIT 0x08    /* active high INPUT0, 1=True, 0=False */
#define RTSTEPPER_STEP_STATE_INPUT1_BIT 0x10    /* active high INPUT1, 1=True, 0=False (1) */
#define RTSTEPPER_STEP_STATE_INPUT2_BIT 0x20    /* active high INPUT2, 1=True, 0=False (1) */
#define RTSTEPPER_STEP_STATE_INPUT3_BIT 0x40    /* active high INPUT3, 1=True, 0=False (1) */
/* (1) Status bits INPUT1-3 are not valid if configured as an ADC input. */

#define RTSTEPPER_MECH_THREAD 1
#define RTSTEPPER_DONGLE_THREAD 0

/* IO request hysteresis set points. */
#define RTSTEPPER_REQ_MAX  100
#define RTSTEPPER_REQ_MIN  50

/* Forward declarations. */
struct emc_session;

#ifdef __cplusplus
extern "C"
{
#endif

/* Function prototypes */

   enum EMC_RESULT rtstepper_open(struct emc_session *ps);
   enum EMC_RESULT rtstepper_close(struct emc_session *ps);
   enum EMC_RESULT rtstepper_state_query(struct emc_session *ps);
   enum EMC_RESULT rtstepper_encode(struct emc_session *ps, struct rtstepper_io_req *io, double index[]);
   enum EMC_RESULT rtstepper_xfr_start(struct emc_session *ps, struct rtstepper_io_req *io, EmcPose pos);
   enum EMC_RESULT rtstepper_xfr_wait(struct emc_session *ps);
   enum EMC_RESULT rtstepper_xfr_hysteresis(struct emc_session *ps);
   enum EMC_RESULT rtstepper_dongle_empty_wait(struct emc_session *ps);
   enum EMC_RESULT rtstepper_dongle_sync_start_wait(struct emc_session *ps);
   enum EMC_RESULT rtstepper_clear_stats(struct emc_session *ps);
   int rtstepper_is_connected(struct emc_session *ps);
   enum EMC_RESULT rtstepper_home(struct emc_session *ps);
   enum EMC_RESULT rtstepper_position_set(struct emc_session *ps, EmcPose pos);
   enum EMC_RESULT rtstepper_estop(struct emc_session *ps, int thread);
   enum EMC_RESULT rtstepper_ctrl_start(struct emc_session *ps, enum STEP_CMD cmd, uint16_t param);
   enum EMC_RESULT rtstepper_state_adc_query(struct emc_session *ps);
   struct rtstepper_io_req *rtstepper_io_req_alloc(struct emc_session *ps, int id, enum RTSTEPPER_IO_TYPE io_type);
   enum EMC_RESULT rtstepper_test(const char *snum);
#ifdef __cplusplus
}
#endif

#endif                          /* _RTSTEPPER_H */
