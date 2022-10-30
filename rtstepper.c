/*****************************************************************************\

  rtstepper.c - rt-stepper dongle support for rtstepperemc

  (c) 2008-2017 Copyright Eckler Software

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

\*****************************************************************************/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <math.h>
#include <unistd.h>
#include <sys/time.h>
#include "emc.h"
#include "ini.h"
#include "bug.h"

#if (defined(__WIN32__) || defined(_WINDOWS))
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define sleep(n) Sleep(1000 * n)
#endif

#define DONGLE_VENDOR_ID 0x04d8
#define DONGLE_PRODUCT_ID 0xff45
#define DONGLE_INTERFACE 0
#define DONGLE_OUT_EP (LIBUSB_ENDPOINT_OUT | 1)

#define LIBUSB_TIMEOUT 30000    /* milliseconds */
#define LIBUSB_CONTROL_REQ_TIMEOUT 5000

static enum EMC_RESULT rtstepper_abort_set(struct emc_session *ps);
static enum EMC_RESULT rtstepper_is_input0_triggered(struct emc_session *ps);
static enum EMC_RESULT rtstepper_is_input1_triggered(struct emc_session *ps);
static enum EMC_RESULT rtstepper_is_input2_triggered(struct emc_session *ps);
static enum EMC_RESULT rtstepper_is_input3_triggered(struct emc_session *ps);

//#define STEP_BUF_CHUNK 4096
#define STEP_BUF_CHUNK 16384

static pthread_mutex_t _mutex = PTHREAD_MUTEX_INITIALIZER;  
static pthread_cond_t _write_done_cond = PTHREAD_COND_INITIALIZER;
static pthread_cond_t _event_done_cond = PTHREAD_COND_INITIALIZER;
static pthread_cond_t _dongle_done_cond = PTHREAD_COND_INITIALIZER;
static pthread_cond_t _dongle_io_done_cond = PTHREAD_COND_INITIALIZER;

static unsigned int step_msg_cnt;
static unsigned int query_msg_cnt;

struct __attribute__ ((packed)) step_state
{
   union
   {
      uint16_t _word;
   };
};

struct __attribute__ ((packed)) step_elements
{
   char reserved[8];
};

struct __attribute__ ((packed)) step_query
{
   struct step_state state_bits;
   uint16_t icount_period;      /* INPUT0 frequency period in counts */
   uint32_t step;               /* running step count */
};

/* ADC = 8-bit resolution, 16mhz / (64 * 4) = 62.5k clock */
struct __attribute__ ((packed)) step_adc
{
   uint8_t input1;  /* ADC 8-bit value */
   uint8_t input2;
   uint8_t input3;
   uint8_t res;
};

struct __attribute__ ((packed)) step_adc_query
{
   struct
   {
      struct step_state state_bits;
      uint16_t icount_period;     /* input0 period in counts */
      struct step_adc adc;        /* ADC input1-3 */
   };
};

static enum EMC_RESULT xfr_start(struct rtstepper_io_req *io);

/* 
 * DB25 pin to bitfield map.
 *                pin #       na, 1,  2,   3,   4,   5,    6,    7,   8,    9  
 */
static const int pin_map[] = { 0, 0, 0x1, 0x2, 0x4, 0x8, 0x10, 0x20, 0x40, 0x80 };

static int is_rt(libusb_device *dev, const char *sn)
{
   struct libusb_device_descriptor desc;
   libusb_device_handle *hd=NULL;
   int r, stat=0, rev;
   char sz[128], prod[32];

   if (libusb_get_device_descriptor(dev, &desc) != 0)
      goto bugout;

   if (!(desc.idVendor == DONGLE_VENDOR_ID && desc.idProduct == DONGLE_PRODUCT_ID))
      goto bugout;

   if ((r = libusb_open(dev, &hd)) != 0)
   {
      BUG("invalid usb_open: %s\n", libusb_error_name(r));
      goto bugout;
   }

   if ((r =libusb_get_string_descriptor_ascii(hd, desc.iProduct, (unsigned char *)prod, sizeof(prod))) < 0)
      goto bugout;
  
   if ((r =libusb_get_string_descriptor_ascii(hd, desc.iSerialNumber, (unsigned char *)sz, sizeof(sz))) < 0)
      goto bugout;
   if (!sz[0])
      strcpy(sz, "0");

   MSG("Found dongle %s serial_number=%s\n", prod, sz);

   if (strncmp(prod+11, "1a", 2) == 0)
      rev = RTSTEPPER_BRD_a;
   else if (strncmp(prod+11, "1b", 2) == 0)
      rev = RTSTEPPER_BRD_b;
   else if (strncmp(prod+11, "1c", 2) == 0)
      rev = RTSTEPPER_BRD_c;
   else if (strncmp(prod+11, "1d", 2) == 0)
      rev = RTSTEPPER_BRD_d;
   else if ((strncmp(prod+11, "1e", 2) == 0) || (strncmp(prod+11, "2e", 2) == 0))
      rev = RTSTEPPER_BRD_e;
   else if (strncmp(prod+11, "3f", 2) == 0)
      rev = RTSTEPPER_BRD_f;
   else
      rev = RTSTEPPER_BRD_NEW;

   stat = rev;    /* found usb device */

 bugout:
   if (hd != NULL)
      libusb_close(hd);

   return stat;
}       /* is_rt() */

/* Libusb requires an event handling thread for asynchronous io. This loop calls xfr_cb(). 
 * Note, libusb_control_transfer() will also call xfr_cb(), this caused a mutex hang issue. 
 */
static void event_thread(struct rtstepper_file_descriptor *pfd)
{
   struct timeval tv;

   tv.tv_sec = 0;
   tv.tv_usec = 0.1 * 1E6;   /* 100ms */

   while (!pfd->event_done)
   {
      /* If libusb has nothing to do, it will block until timeout expires. */
      libusb_handle_events_timeout_completed(pfd->ctx, &tv, NULL);
   }  /* while (!event_done) */

   DBG("event_thread() closed...\n");
   pfd->event_abort_done = 1;
   pthread_cond_signal(&_event_done_cond);
}  /* event_thread() */

static void dongle_thread(struct rtstepper_file_descriptor *pfd)
{
   struct emc_session *ps;
   struct rtstepper_ctrl_req *io;
   enum EMC_RESULT r;
   int len, empty;

   ps = container_of(pfd, struct emc_session, fd_table);

   while (!pfd->dongle_done)
   {
      esleep(0.05);   /* 50ms */

      if (ps->fd_table.board_rev > RTSTEPPER_BRD_e)
         r = rtstepper_state_adc_query(ps);
      else
         r = rtstepper_state_query(ps);

      //BUG("state_bits: %x\n", ps->state_bits);

      if (r != EMC_R_OK)
      {
         if (r == RTSTEPPER_R_DEVICE_UNAVAILABLE)
         {
            emc_estop_post_cb(ps);
            MSG("DEVICE_DISCONNECT estop...\n");
            goto bugout;
         }
      }

      if (rtstepper_is_input0_triggered(ps) == RTSTEPPER_R_INPUT_TRUE)
      {
         rtstepper_estop(ps, RTSTEPPER_DONGLE_THREAD);
         emc_estop_post_cb(ps);
         MSG("INPUT0 estop...\n");
      }
      if (rtstepper_is_input1_triggered(ps) == RTSTEPPER_R_INPUT_TRUE)
      {
         rtstepper_estop(ps, RTSTEPPER_DONGLE_THREAD);
         emc_estop_post_cb(ps);
         MSG("INPUT1 estop...\n");
      }
      if (rtstepper_is_input2_triggered(ps) == RTSTEPPER_R_INPUT_TRUE)
      {
         rtstepper_estop(ps, RTSTEPPER_DONGLE_THREAD);
         emc_estop_post_cb(ps);
         MSG("INPUT2 estop...\n");
      }
      if (rtstepper_is_input3_triggered(ps) == RTSTEPPER_R_INPUT_TRUE)
      {
         rtstepper_estop(ps, RTSTEPPER_DONGLE_THREAD);
         emc_estop_post_cb(ps);
         MSG("INPUT3 estop...\n");
      }

      pthread_mutex_lock(&_mutex);

      empty = list_empty(&ps->ctrl_head.list);
      if (!empty)
      {
         io = list_entry(ps->ctrl_head.list.next, struct rtstepper_ctrl_req, list);
         list_del(&io->list);
      }

      pthread_mutex_unlock(&_mutex);

      if (!empty)
      {
         len = libusb_control_transfer(ps->fd_table.hd, LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_INTERFACE,      /* bmRequestType */
                         io->cmd,        /* bRequest */
                         io->param,   /* wValue */
                         DONGLE_INTERFACE, /* wIndex */
                         NULL, 0, LIBUSB_CONTROL_REQ_TIMEOUT);
         free(io);

         if (len < 0)
            BUG("invalid usb_ctrl_transfer ret=%d: %s\n", len, libusb_error_name(len));
      }

      pthread_cond_signal(&_dongle_io_done_cond);

   }  /* while (!dongle_done) */

bugout:
   DBG("dongle_thread() closed...\n");
   pfd->dongle_abort_done = 1;
   pthread_cond_signal(&_dongle_done_cond);
}  /* dongle_thread() */

static int claim_interface(struct rtstepper_file_descriptor *pfd)
{
   int stat=1, r;

   pthread_mutex_lock(&_mutex);

   if (pfd->hd != NULL)
   {
      BUG("invalid state, interface is already claimed\n");
      goto bugout;  /* interface is already claimed */
   }

   if ((r = libusb_open(pfd->dev, &pfd->hd)) != 0)
   {
      BUG("invalid libusb_open: %s\n", libusb_error_name(r));
      goto bugout;
   }
      
   if ((r = libusb_claim_interface(pfd->hd, DONGLE_INTERFACE)) != 0)
   {
      libusb_close(pfd->hd);
      pfd->hd = NULL;
      BUG("invalid claim_interface: %s\n", libusb_error_name(r));
      goto bugout;
   }

   DBG("claimed interface %d\n", DONGLE_INTERFACE);

   stat=0;

bugout:
   pthread_mutex_unlock(&_mutex);
   return stat;   
} /* claim_interface() */

static int release_interface(struct rtstepper_file_descriptor *pfd)
{
   if (pfd->hd == NULL)
      return 0;

   pthread_mutex_lock(&_mutex);

   libusb_release_interface(pfd->hd, DONGLE_INTERFACE);
   libusb_close(pfd->hd);
   libusb_free_device_list(pfd->list_all, 1);
   libusb_exit(pfd->ctx);
   pfd->hd = NULL;

   DBG("released interface %d\n", DONGLE_INTERFACE);

   pthread_mutex_unlock(&_mutex);
   return 0;
} /* release_interface() */

static enum EMC_RESULT close_device(struct rtstepper_file_descriptor *pfd)
{
   if (pfd->hd != NULL)
   {
      /* Wait for dongle_thread to shutdown before calling libusb_close. */
      pfd->dongle_done = 1;
      pthread_mutex_lock(&_mutex);
      while (!pfd->dongle_abort_done)
         pthread_cond_wait(&_dongle_done_cond, &_mutex);
      pthread_mutex_unlock(&_mutex);

      /* Wait for event_thread to shutdown before calling libusb_close. */
      pfd->event_done = 1;
      pthread_mutex_lock(&_mutex);
      while (!pfd->event_abort_done)
         pthread_cond_wait(&_event_done_cond, &_mutex);
      pthread_mutex_unlock(&_mutex);

      release_interface(pfd);
   }
   return EMC_R_OK;
}       /* close_device() */

static enum EMC_RESULT open_device(struct rtstepper_file_descriptor *pfd, const char *sn)
{
   enum EMC_RESULT stat = RTSTEPPER_R_DEVICE_UNAVAILABLE;
   int i, n, rev;
   
   libusb_init(&pfd->ctx);
   libusb_set_debug(pfd->ctx, 3);
   n = libusb_get_device_list(pfd->ctx, &pfd->list_all);

   /* Look for rtstepper device(s). */
   for (i=0; i < n; i++)
   {
      if ((rev = is_rt(pfd->list_all[i], sn)))
      {
         pfd->dev = pfd->list_all[i];
         if (claim_interface(pfd))
            goto bugout;

         pfd->board_rev = rev;

         /* Create event_thread after libusb_open. */
         pfd->event_done = pfd->event_abort_done = 0;
         pthread_create(&pfd->event_tid, NULL, (void *(*)(void *))event_thread, (void *)pfd);

         /* Create dongle_thread for reading input bits and writing output bits. */
         pfd->dongle_done = pfd->dongle_abort_done = 0;
         pthread_create(&pfd->dongle_tid, NULL, (void *(*)(void *))dongle_thread, (void *)pfd);

         stat = EMC_R_OK;
         break;
      }
   }

bugout:
   return stat;
}       /* open_device() */

static void xfr_cancel(struct emc_session *ps)
{
   struct rtstepper_io_req *io;
   struct list_head *p, *tmp;

   pthread_mutex_lock(&_mutex);

   /* Walk the queue deleting all io requests. */
   list_for_each_safe(p, tmp, &ps->head.list)
   {
      /* Need to re-home after canceling any io request. */
      ps->state_bits &= ~EMC_STATE_HOMED_BIT;

      io = list_entry(p, struct rtstepper_io_req, list);

      /* Cancel current io transfer, xfr_cb() will complete the cancel.  */
      if (io->req != NULL)
      {
         libusb_cancel_transfer(io->req);
         continue;
      }
      
      /* Remove all pending io requests from the queue. */
      free(io->buf);
      list_del(&io->list);
      free(io);
   }

   ps->req_cnt = 0;  /* force zero cnt here */

   pthread_mutex_unlock(&_mutex);
} /* xfr_cancel() */

/* Called by xfr_start(). Assums no current IOs are active. */
static void xfr_sync_cancel(struct emc_session *ps)
{   
   struct rtstepper_io_req *io;
   struct list_head *p, *tmp;

   pthread_mutex_lock(&_mutex);

   /* Walk the queue deleting all io requests. */
   list_for_each_safe(p, tmp, &ps->head.list)
   {
      io = list_entry(p, struct rtstepper_io_req, list);
      
      /* Remove all pending io requests from the queue. */
      free(io->buf);
      list_del(&io->list);
      free(io);
      ps->req_cnt--;
   }

   pthread_mutex_unlock(&_mutex);
}  /* xfr_sync_cancel() */

#if 0
static int x_index;
static int y_index;
static int z_index;

static void bitchk(int id, unsigned char *buf, int total)
{
//   static const unsigned int inc[8] = {0, -1, 0, 1, 0, 0, 0, 0};  /* high true */
   static const unsigned int inc[8] = {0, 0, 0, 0, -1, 0, 1, 0};   /* low true */
   int i,x=0,y=0,z=0;
  
   for (i=0; i<total; i++)
   {
      x = (x << 2) & 0x7;
      if (buf[i] & 0x1)
         x |= 1;
      if (buf[i] & 0x2)
         x |= 2;
      x_index += inc[x];

      y = (y << 2) & 0x7;
      if (buf[i] & 0x4)
         y |= 1;
      if (buf[i] & 0x8)
         y |= 2;
      y_index += inc[y];

      z = (z << 2) & 0x7;
      if (buf[i] & 0x10)
         z |= 1;
      if (buf[i] & 0x20)
         z |= 2;
      z_index += inc[z];
      //fprintf(stdout, " %d %x\n", id, buf[i]);
   }
   DBG("BITCHK l=%d x=%0.5f x_index=%d y=%0.5f y_index=%d z=%0.5f z_index=%d\n", id, 
      1/32000.0 * x_index, x_index,
      1/32000.0 * y_index, y_index,
      1/32000.0 * z_index, z_index);   
}
#endif

/* Libusb asynchronous transfer complete callback function. Called from the event_thread(). */
static void xfr_cb(struct libusb_transfer *transfer)
{
   struct emc_session *ps;
   struct rtstepper_io_req *io;
   int empty;

   io = transfer->user_data;
   ps = io->session;

   DBG("xfr_cb() io=%p, line=%d, req_cnt=%d, %d bytes written, x=%0.5f, y=%0.5f, z=%0.5f, a=%0.5f, b=%0.5f, c=%0.5f\n", 
      io, io->id, ps->req_cnt, transfer->actual_length, io->position.tran.x, io->position.tran.y, io->position.tran.z,
      io->position.a, io->position.b, io->position.c);

   if (transfer->status != LIBUSB_TRANSFER_COMPLETED)
   {
      switch (transfer->status)
      {
      case LIBUSB_TRANSFER_ERROR:
         BUG("usb transfer failed\n");
         emc_estop_post_cb(ps);
         break;
      case LIBUSB_TRANSFER_TIMED_OUT:
         BUG("usb transfer timed out\n");
         emc_estop_post_cb(ps);
         break;
      case LIBUSB_TRANSFER_CANCELLED:
         MSG("Usb transfer was cancelled\n");
         break;
      case LIBUSB_TRANSFER_STALL:
         BUG("usb transfer stalled\n");
         emc_estop_post_cb(ps);
         break;
      case LIBUSB_TRANSFER_NO_DEVICE:
         BUG("usb device was disconnected\n");
         emc_estop_post_cb(ps);
         break;
      case LIBUSB_TRANSFER_OVERFLOW:
         BUG("usb transfer overflow\n");
         emc_estop_post_cb(ps);
         break;
      default:
         BUG("invalid usb transfer: %d\n", transfer->status);
         break;
      }
   }
   else
   {
      if (transfer->actual_length != transfer->length)
      {
         BUG("usb transfer incomplete exp=%d act=%d\n", transfer->length, transfer->actual_length);
         emc_estop_post_cb(ps);
      }
      else
      {
         //bitchk(io->id, io->buf, io->total);

         /* Transfer is ok, save current position. */
         ps->position = io->position;
         emc_position_post_cb(io->id, io->position); 
      }
   }

   pthread_mutex_lock(&_mutex);

   libusb_free_transfer(io->req);
   free(io->buf);
   list_del(&io->list);
   free(io);
   ps->req_cnt--;
   empty = list_empty(&ps->head.list);

   pthread_mutex_unlock(&_mutex);

   if (!empty)
   {
      /* Kickoff next usb io request from the head of the queue (FIFO). */
      io = list_entry(ps->head.list.next, struct rtstepper_io_req, list);
      if (xfr_start(io) != EMC_R_OK)
      {
         DBG("IO canceled broadcast write_done_cond...\n");
         pthread_cond_broadcast(&_write_done_cond);
      }
   } 
   else
   { 
      /* All usb io is complete. */
      DBG("broadcast write_done_cond...\n");
      pthread_cond_broadcast(&_write_done_cond);
   }

   return;
}  /* xfr_cb() */

static enum EMC_RESULT xfr_start(struct rtstepper_io_req *io)
{
   enum EMC_RESULT stat = RTSTEPPER_R_IO_ERROR;
   struct emc_session *ps = io->session;
   int r, tmo;

   if (ps->state_bits & EMC_STATE_ESTOP_BIT)
   {
      esleep(0.07);   /* Wait 70ms for rtstepper_estop() to complete. */
      return RTSTEPPER_R_IO_CANCELED;  /* ESTOP active, ignore io requests. */
   }

   if (ps->state_bits & EMC_STATE_CANCEL_BIT)
   {
      esleep(0.05);   /* Wait 50ms for dsp_auto() thread to cancel. */
      xfr_sync_cancel(ps);  /* remove all io requests */
      return RTSTEPPER_R_IO_CANCELED; 
   }

   DBG("xfr_start() io=%p, line=%d, cnt=%d, req_cnt=%d\n", io, io->id, io->total, ps->req_cnt);

   tmo = (int)((double) io->total * 0.021333);      /* timeout in ms = steps * period * 1000 */
   tmo += 5000; /* plus 5 seconds */

   /* Allocate an asynchronous transfer. */
   io->req = libusb_alloc_transfer(0);
   libusb_fill_bulk_transfer(io->req, ps->fd_table.hd, DONGLE_OUT_EP, io->buf, io->total, xfr_cb, io, tmo);

   /* Kickoff the asynchronous io. */
   if ((r = libusb_submit_transfer(io->req)) != 0)
   {
      BUG("invalid start_xfr: %s\n", libusb_error_name(r));
      emc_estop_post_cb(ps);
      goto bugout;
   }

   ps->current_io_type = io->type;

   stat = EMC_R_OK;
bugout:
   return stat;
}  /* xfr_start() */

enum EMC_RESULT rtstepper_xfr_start(struct emc_session *ps, struct rtstepper_io_req *io, EmcPose pos)
{
   enum EMC_RESULT stat = RTSTEPPER_R_IO_ERROR;
   int i, j, mid, empty;

   if (io == NULL)
      goto bugout;

   DBG("rtstepper_xfr_start: line=%d x_index=%d y_index=%d z_index=%d a_index=%d b_index=%d c_index=%d io=%p cnt=%d\n", io->id, ps->axis[0].master_index,
       ps->axis[1].master_index, ps->axis[2].master_index, ps->axis[3].master_index, ps->axis[4].master_index, ps->axis[5].master_index, io, io->total);

   /* Save commanded position for this io request. */
   io->position = pos;

   /* Save index(s) here for position update after user cancel?? Position after cancel does not account for backlash compensation. */ 

   /* Finish last pulse for this step buffer. */
   for (i = 0; i < ps->axes; i++)
   {
      if (ps->axis[i].step_pin == 0)
         continue;   /* skip */

      if (ps->axis[i].clk_tail)
      {
         /* Stretch pulse to 50% duty cycle. */
         mid = (io->total - ps->axis[i].clk_tail) / 2;
         for (j=0; j < mid; j++)
         {
            if (ps->axis[i].step_active_high)
               io->buf[ps->axis[i].clk_tail + j] |= pin_map[ps->axis[i].step_pin]; /* set bit */
            else
               io->buf[ps->axis[i].clk_tail + j] &= ~pin_map[ps->axis[i].step_pin]; /* clear bit */
         }
         ps->axis[i].clk_tail = 0;  /* reset */
      }
   }

   pthread_mutex_lock(&_mutex);

   empty = list_empty(&ps->head.list);

   /* Add io request to tail of the queue (FIFO). */
   list_add_tail(&io->list, &ps->head.list);
   ps->req_cnt++;

   pthread_mutex_unlock(&_mutex);

   if (empty)
   {
      /* Kick off first usb io request here. */
      xfr_start(io);
   }

   stat = EMC_R_OK;

 bugout:
   return stat;
}       /* rtstepper_xfr_start() */

enum EMC_RESULT rtstepper_ctrl_start(struct emc_session *ps, enum STEP_CMD cmd, uint16_t param)
{
   enum EMC_RESULT stat = RTSTEPPER_R_IO_ERROR;
   struct rtstepper_ctrl_req *io;

   if (ps->fd_table.hd == NULL)
      return stat;  /* no usb dongle available */

   if ((io = malloc(sizeof(struct rtstepper_ctrl_req))) == NULL)
      return stat;

   io->cmd = cmd;
   io->param = param;

   pthread_mutex_lock(&_mutex);

   /* Add io request to tail of the queue (FIFO) for the dongle_thread. */
   list_add_tail(&io->list, &ps->ctrl_head.list);

   pthread_mutex_unlock(&_mutex);
   return EMC_R_OK;
}

/* Apply xfr hysteresis. */
enum EMC_RESULT rtstepper_xfr_hysteresis(struct emc_session *ps)
{
   struct timeval tv;
   struct timespec ts;
   int rc;

   if (ps->req_cnt > RTSTEPPER_REQ_MAX)
   {
      DBG("rstepper_xfr_hysteresis() start...\n");

      /* Wait until IOs fall below minimum set point. */
      do
      {
         gettimeofday(&tv, NULL);
         ts.tv_sec = tv.tv_sec + 2;    /* 2 sec timeout */
         ts.tv_nsec = 0;
         rc=0;
         pthread_mutex_lock(&_mutex);
         while (ps->req_cnt > RTSTEPPER_REQ_MIN && rc==0)
            rc = pthread_cond_timedwait(&_write_done_cond, &_mutex, &ts);
         pthread_mutex_unlock(&_mutex);
      } while (rc == ETIMEDOUT);

      DBG("rstepper_xfr_hysteresis() done...\n");
   }
   return EMC_R_OK;
}   /* rtstepper_xfr_hysteresis() */

#if 0
/* Wait for IO to finish, EStop or Cancel. */
enum EMC_RESULT rtstepper_xfr_cancel_wait(struct emc_session *ps)
{
   struct timeval tv;
   struct timespec ts;
   int rc;

   DBG("rstepper_xfr_cancel_wait() start...\n");

   do
   {
      gettimeofday(&tv, NULL);
      ts.tv_sec = tv.tv_sec + 2;    /* 2 sec timeout */
      ts.tv_nsec = 0;
      rc=0;
      pthread_mutex_lock(&_mutex);
      while (list_empty(&ps->head.list)==0 && (ps->state_bits & EMC_STATE_ESTOP_BIT)==0 && (ps->state_bits & EMC_STATE_CANCEL_BIT)==0 && rc==0)
        rc = pthread_cond_timedwait(&_write_done_cond, &_mutex, &ts);
      pthread_mutex_unlock(&_mutex);
   } while (rc == ETIMEDOUT);

   DBG("rstepper_xfr_cancel_wait() done...\n");

   return EMC_R_OK;
}   /* rtstepper_xfr_cancel_wait() */
#endif

/* Wait for IO to finish. */
enum EMC_RESULT rtstepper_xfr_wait(struct emc_session *ps)
{
   struct timeval tv;
   struct timespec ts;
   int rc;

   /* Wait for all IO to finish. */
   DBG("rstepper_xfr_wait()\n");

   /*
    * Use ptread_cond_timedwait() here because spurious wakeups are not guaranteed. 
    * This means predicates may never get checked after pthread_cond_wait() thus
    * causing a potential hang.
    */
   do
   {
      gettimeofday(&tv, NULL);
      ts.tv_sec = tv.tv_sec + 2;    /* 2 sec timeout */
      ts.tv_nsec = 0;
      rc=0;
      pthread_mutex_lock(&_mutex);
      while (list_empty(&ps->head.list)==0 && rc==0)
         rc = pthread_cond_timedwait(&_write_done_cond, &_mutex, &ts);
      pthread_mutex_unlock(&_mutex);
   } while (rc == ETIMEDOUT);

   DBG("rstepper_xfr_wait() done...\n");
   
   return EMC_R_OK;
}   /* rtstepper_xfr_wait() */

/* Wait for dongle empty bit to be true. Used by interpreter. */
enum EMC_RESULT rtstepper_dongle_empty_wait(struct emc_session *ps)
{
   struct timeval tv;
   struct timespec ts;
   int rc;

   DBG("rtstepper_dongle_empty_wait() start...\n");

   do
   {
      gettimeofday(&tv, NULL);
      ts.tv_sec = tv.tv_sec + 2;    /* 2 sec timeout */
      ts.tv_nsec = 0;
      rc=0;
      pthread_mutex_lock(&_mutex);
      while ((ps->state_bits & RTSTEPPER_STEP_STATE_EMPTY_BIT)==0 && (ps->state_bits & EMC_STATE_ESTOP_BIT)==0 && (ps->state_bits & EMC_STATE_CANCEL_BIT)==0 && rc==0)
        rc = pthread_cond_timedwait(&_dongle_io_done_cond, &_mutex, &ts);
      pthread_mutex_unlock(&_mutex);
   } while (rc == ETIMEDOUT);

   DBG("rtstepper_dongle_empty_wait() done...\n");

   return EMC_R_OK;
}   /* rtstepper_dongle_empty_wait() */

/* Wait for dongle sync_start bit to be true. Used by interpreter. */
enum EMC_RESULT rtstepper_dongle_sync_start_wait(struct emc_session *ps)
{
   struct timeval tv;
   struct timespec ts;
   int rc;

   DBG("rtstepper_dongle_sync_start_wait() start...\n");

   do
   {
      gettimeofday(&tv, NULL);
      ts.tv_sec = tv.tv_sec + 2;    /* 2 sec timeout */
      ts.tv_nsec = 0;
      rc=0;
      pthread_mutex_lock(&_mutex);
      while ((ps->state_bits & RTSTEPPER_STEP_STATE_SYNC_START_BIT)==0 && (ps->state_bits & EMC_STATE_ESTOP_BIT)==0 && (ps->state_bits & EMC_STATE_CANCEL_BIT)==0 && rc==0)
        rc = pthread_cond_timedwait(&_dongle_io_done_cond, &_mutex, &ts);
      pthread_mutex_unlock(&_mutex);
   } while (rc == ETIMEDOUT);

   DBG("rtstepper_dongle_sync_start_wait() done...\n");

   return EMC_R_OK;
}   /* rtstepper_dongle_sync_start_wait() */

struct rtstepper_io_req *rtstepper_io_req_alloc(struct emc_session *ps, int id, enum RTSTEPPER_IO_TYPE io_type)
{
   struct rtstepper_io_req *io;
   
   if (ps->fd_table.hd == NULL)
      return NULL;  /* no usb dongle available */
   if (ps->state_bits & EMC_STATE_ESTOP_BIT)
      return NULL;  /* ESTOP active, ignore io requests. */

   if ((io = malloc(sizeof(struct rtstepper_io_req))) != NULL)
   {
      io->id = id;
      io->buf = NULL;
      io->buf_size = 0;
      io->total = 0;
      io->session = ps;
      io->req = NULL;
      io->type = io_type;
   }
   return io;
}  /* rtstepper_io_req_alloc() */

/*
 * Given a command position in counts for each axis, encode each value into a single step/direction byte. 
 * Store the byte in buffer that is big enough to hold a complete stepper motor move.
 */
enum EMC_RESULT rtstepper_encode(struct emc_session *ps, struct rtstepper_io_req *io, double index[])
{
   int i, j, step, mid, new_size, stat = RTSTEPPER_R_MALLOC_ERROR;
   unsigned char *tmp;

   if (io == NULL)
      goto bugout;

   if (io->buf == NULL || (io->buf_size - io->total) < 2)
   {
      new_size = (io->buf_size < STEP_BUF_CHUNK) ? STEP_BUF_CHUNK : io->buf_size * 2;
      if ((tmp = (unsigned char *)realloc(io->buf, new_size)) == NULL)
      {
         free(io->buf);
         io->buf = NULL;
         BUG("unable to malloc step buffer size=%d\n", new_size);
         goto bugout;   /* bail */
      }
      io->buf = tmp;
      io->buf_size = new_size;
   }

   for (i = 0; i < ps->axes; i++)
   {
      /* Check DB25 pin assignments for this axis, if no pins are assigned skip this axis. Useful for XYZABC axes where AB are unused. */
      if (ps->axis[i].step_pin == 0 || ps->axis[i].direction_pin == 0)
         continue;   /* skip */

      /* Set step bit to default state, high if low_true logic or low if high_true logic. */
      if (ps->axis[i].step_active_high)
      {
         io->buf[io->total] &= ~pin_map[ps->axis[i].step_pin];
         io->buf[io->total + 1] &= ~pin_map[ps->axis[i].step_pin];
      }
      else
      {
         io->buf[io->total] |= pin_map[ps->axis[i].step_pin];
         io->buf[io->total + 1] |= pin_map[ps->axis[i].step_pin];
      }

      /* Set direction bit to default state, high if low_true logic or low if high_true logic. */
      if (ps->axis[i].direction_active_high)
      {
         io->buf[io->total] &= ~pin_map[ps->axis[i].direction_pin];
         io->buf[io->total + 1] &= ~pin_map[ps->axis[i].direction_pin];
      }
      else
      {
         io->buf[io->total] |= pin_map[ps->axis[i].direction_pin];
         io->buf[io->total + 1] |= pin_map[ps->axis[i].direction_pin];
      }

      /* Calculate the step pulse for this clock cycle */
      step = round(index[i] * ps->axis[i].steps_per_unit) - ps->axis[i].master_index;

      if (step < -1 || step > 1)
      {
         if (step_msg_cnt++ < 5)
         {
            BUG("invalid step value (run All Zero): id=%d axis=%d cmd_pos=%0.8f master_index=%d input_scale=%0.2f step=%d\n",
                io->id, i, index[i], ps->axis[i].master_index, ps->axis[i].steps_per_unit, step);
         }
         step = 0;
      }

      if (step)
      {
         /* Got a valid step pulse this cycle. */
         if (ps->axis[i].clk_tail)
         {
            /* Using the second pulse, stretch pulse to 50% duty cycle. */
            mid = (io->total - ps->axis[i].clk_tail) / 2;
            for (j=0; j < mid; j++)
            {
               if (ps->axis[i].step_active_high)
                  io->buf[ps->axis[i].clk_tail + j] |= pin_map[ps->axis[i].step_pin]; /* set bit */
               else
                  io->buf[ps->axis[i].clk_tail + j] &= ~pin_map[ps->axis[i].step_pin]; /* clear bit */
            }
         }

         /* save old step location */
         ps->axis[i].clk_tail = io->total;

         /* save step direction */
         ps->axis[i].direction = step;
      }

      /* Set direction bit. */
      if (ps->axis[i].direction < 0)
      {
         if (ps->axis[i].direction_active_high)
         {
            io->buf[io->total] |= pin_map[ps->axis[i].direction_pin];    /* set bit */
            io->buf[io->total + 1] |= pin_map[ps->axis[i].direction_pin];  /* set bit */
         }
         else
         {
            io->buf[io->total] &= ~pin_map[ps->axis[i].direction_pin];       /* clear bit */
            io->buf[io->total + 1] &= ~pin_map[ps->axis[i].direction_pin];   /* clear bit */
         }
      }

      ps->axis[i].master_index += step;

//      DBG("axis=%d index=%0.6f master_index=%d\n", i, index[i] * ps->axis[i].steps_per_unit, ps->axis[i].master_index);
   }    /* for (i=0; i < num_axis; i++) */

   io->total += 2;

   stat = EMC_R_OK;

 bugout:
   return stat;
}       /* rtstepper_encode() */

int rtstepper_is_connected(struct emc_session *ps)
{
   return ps->fd_table.hd != NULL;
}       /* rtstepper_is_connected() */

enum EMC_RESULT rtstepper_position_set(struct emc_session *ps, EmcPose pos)
{
   double new_pos_cmd[EMC_MAX_AXIS];
   int i;

   DBG("rtstepper_position_set() x=%0.5f y=%0.5f z=%0.5f a=%0.5f\n", pos.tran.x, pos.tran.y, pos.tran.z, pos.a);
#if 0
   ps->axis[EMC_AXIS_X].master_index = round(pos.tran.x * ps->axis[EMC_AXIS_X].steps_per_unit);
   ps->axis[EMC_AXIS_Y].master_index = round(pos.tran.y * ps->axis[EMC_AXIS_Y].steps_per_unit);
   ps->axis[EMC_AXIS_Z].master_index = round(pos.tran.z * ps->axis[EMC_AXIS_Z].steps_per_unit);
   ps->axis[EMC_AXIS_A].master_index = round(pos.a * ps->axis[EMC_AXIS_A].steps_per_unit);
   ps->axis[EMC_AXIS_B].master_index = round(pos.b * ps->axis[EMC_AXIS_B].steps_per_unit);
   ps->axis[EMC_AXIS_C].master_index = round(pos.c * ps->axis[EMC_AXIS_C].steps_per_unit);
   ps->axis[EMC_AXIS_U].master_index = round(pos.u * ps->axis[EMC_AXIS_U].steps_per_unit);
   ps->axis[EMC_AXIS_V].master_index = round(pos.v * ps->axis[EMC_AXIS_V].steps_per_unit);
   ps->axis[EMC_AXIS_W].master_index = round(pos.w * ps->axis[EMC_AXIS_W].steps_per_unit);
#endif

   /* Convert coordinates into array. */
   new_pos_cmd[EMC_AXIS_X] = pos.tran.x;
   new_pos_cmd[EMC_AXIS_Y] = pos.tran.y;
   new_pos_cmd[EMC_AXIS_Z] = pos.tran.z;
   new_pos_cmd[EMC_AXIS_A] = pos.a;
   new_pos_cmd[EMC_AXIS_B] = pos.b;
   new_pos_cmd[EMC_AXIS_C] = pos.c;
   new_pos_cmd[EMC_AXIS_U] = pos.u;
   new_pos_cmd[EMC_AXIS_V] = pos.v;
   new_pos_cmd[EMC_AXIS_W] = pos.w;

   /* Map new coordinate position to each axis (step/dir pins). One coordinate may map to more than one axis. */
   for (i = 0; i < ps->axes; i++)
   {
      ps->axis[i].pos_cmd = new_pos_cmd[ps->axis[i].coordinate_map];
      ps->axis[i].master_index = round(new_pos_cmd[ps->axis[i].coordinate_map] * ps->axis[i].steps_per_unit);
   }

   return EMC_R_OK;
}       /* rtstepper_position_set() */

enum EMC_RESULT rtstepper_estop(struct emc_session *ps, int thread)
{
   struct rtstepper_file_descriptor *pfd = &ps->fd_table;
   enum EMC_RESULT stat = RTSTEPPER_R_REQ_ERROR;

   ps->state_bits |= EMC_STATE_ESTOP_BIT;
   DBG("rtstepper_estop()\n");

   if (pfd->hd == NULL)
      goto bugout;

   if (thread == RTSTEPPER_MECH_THREAD)
   {
      /* User EStop, wait for dongle_thread to shutdown before sending abort. */
      pfd->dongle_done = 1;
      pthread_mutex_lock(&_mutex);
      while (!pfd->dongle_abort_done)
         pthread_cond_wait(&_dongle_done_cond, &_mutex);
      pthread_mutex_unlock(&_mutex);
   }

   rtstepper_abort_set(ps);   /* EReset clears the dongle Abort bit. */
   xfr_cancel(ps);
   
   stat = EMC_R_OK;

bugout:
   return stat;
}       /* rtstepper_estop() */

/* Set abort state in dongle. */
static enum EMC_RESULT rtstepper_abort_set(struct emc_session *ps)
{
   enum EMC_RESULT stat;
   int len;

   DBG("rtstepper_abort_set() state=%x\n", ps->state_bits);

   if (ps->fd_table.hd == NULL)
   {
      stat = RTSTEPPER_R_REQ_ERROR;
      goto bugout;
   }

   len = libusb_control_transfer(ps->fd_table.hd, LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_INTERFACE,      /* bmRequestType */
                         STEP_ABORT_SET,        /* bRequest */
                         0x0,   /* wValue */
                         DONGLE_INTERFACE, /* wIndex */
                         NULL, 0, LIBUSB_CONTROL_REQ_TIMEOUT);

   if (len < 0)
   {
      BUG("set_abort failed ret=%d: %s\n", len, libusb_error_name(len));
      stat = RTSTEPPER_R_IO_ERROR;
      goto bugout;
   }

   stat = EMC_R_OK;

 bugout:
   return stat;
}       /* rtstepper_abort_set() */

#if 0
/* Clear abort state in dongle. */
enum EMC_RESULT rtstepper_abort_clear(struct emc_session *ps)
{
   enum EMC_RESULT stat;
   int len;

   if (ps->fd_table.hd == NULL)
   {
      stat = RTSTEPPER_R_REQ_ERROR;
      goto bugout;
   }

   len = libusb_control_transfer(ps->fd_table.hd, LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_INTERFACE,      /* bmRequestType */
                         STEP_ABORT_CLEAR,      /* bRequest */
                         0x0,   /* wValue */
                         DONGLE_INTERFACE, /* wIndex */
                         NULL, 0, LIBUSB_CONTROL_REQ_TIMEOUT);

   if (len < 0)
   {
      BUG("clear_abort failed ret=%d: %s\n", len, libusb_error_name(len));
      stat = RTSTEPPER_R_IO_ERROR;
      goto bugout;
   }

   /* Reset step_state_inputx_bit for rtstepper_is_inputx_triggered(). */
   ps->old_state_bits &= ~(RTSTEPPER_STEP_STATE_INPUT0_BIT | RTSTEPPER_STEP_STATE_INPUT1_BIT | RTSTEPPER_STEP_STATE_INPUT2_BIT);

   stat = EMC_R_OK;

 bugout:
   return stat;
}       /* rtstepper_abort_clear() */
#endif

enum EMC_RESULT rtstepper_is_input0_triggered(struct emc_session *ps)
{
   enum EMC_RESULT stat = RTSTEPPER_R_INPUT_FALSE;
   int new_bit, old_bit;

   if (ps->fd_table.hd == NULL)
      goto bugout;

   old_bit = ps->old_state_bits & RTSTEPPER_STEP_STATE_INPUT0_BIT;
   if (ps->input0_abort_enabled)
   {
      new_bit = ps->state_bits & RTSTEPPER_STEP_STATE_INPUT0_BIT;
      if ((new_bit == RTSTEPPER_STEP_STATE_INPUT0_BIT) && (old_bit == 0))
      {
         /* Found input0 low to high transition. */
         stat = RTSTEPPER_R_INPUT_TRUE;
      }
      ps->old_state_bits |= new_bit;
   }

 bugout:
   return stat;
}       /* rtstepper_is_input0_triggered() */

enum EMC_RESULT rtstepper_is_input1_triggered(struct emc_session *ps)
{
   enum EMC_RESULT stat = RTSTEPPER_R_INPUT_FALSE;
   int new_bit, old_bit;

   if (ps->fd_table.hd == NULL)
      goto bugout;

   old_bit = ps->old_state_bits & RTSTEPPER_STEP_STATE_INPUT1_BIT;
   if (ps->input1_abort_enabled)
   {
      new_bit = ps->state_bits & RTSTEPPER_STEP_STATE_INPUT1_BIT;
      if ((new_bit == RTSTEPPER_STEP_STATE_INPUT1_BIT) && (old_bit == 0))
      {
         /* Found input1 low to high transition. */
         stat = RTSTEPPER_R_INPUT_TRUE;
      }
      ps->old_state_bits |= new_bit;
   }

 bugout:
   return stat;
}       /* rtstepper_is_input1_triggered() */

enum EMC_RESULT rtstepper_is_input2_triggered(struct emc_session *ps)
{
   enum EMC_RESULT stat = RTSTEPPER_R_INPUT_FALSE;
   int new_bit, old_bit;

   if (ps->fd_table.hd == NULL)
      goto bugout;

   old_bit = ps->old_state_bits & RTSTEPPER_STEP_STATE_INPUT2_BIT;
   if (ps->input2_abort_enabled)
   {
      new_bit = ps->state_bits & RTSTEPPER_STEP_STATE_INPUT2_BIT;
      if ((new_bit == RTSTEPPER_STEP_STATE_INPUT2_BIT) && (old_bit == 0))
      {
         /* Found input2 low to high transition. */
         stat = RTSTEPPER_R_INPUT_TRUE;
      }
      ps->old_state_bits |= new_bit;
   }

 bugout:
   return stat;
}       /* rtstepper_is_input2_triggered() */

enum EMC_RESULT rtstepper_is_input3_triggered(struct emc_session *ps)
{
   enum EMC_RESULT stat = RTSTEPPER_R_INPUT_FALSE;
   int new_bit, old_bit;

   if (ps->fd_table.hd == NULL)
      goto bugout;

   old_bit = ps->old_state_bits & RTSTEPPER_STEP_STATE_INPUT3_BIT;
   if (ps->input3_abort_enabled)
   {
      new_bit = ps->state_bits & RTSTEPPER_STEP_STATE_INPUT3_BIT;
      if ((new_bit == RTSTEPPER_STEP_STATE_INPUT3_BIT) && (old_bit == 0))
      {
         /* Found input3 low to high transition. */
         stat = RTSTEPPER_R_INPUT_TRUE;
      }
      ps->old_state_bits |= new_bit;
   }

 bugout:
   return stat;
}       /* rtstepper_is_input3_triggered() */

/* Use moving average as a low-pass filter on the icount samples. */ 
static uint16_t icount_moving_avg(struct emc_session *ps, struct rtstepper_moving_avg *ma, int new_count)
{
  int len = sizeof(ma->array) / sizeof(ma->array[0]);

  /* Subtract the oldest number from the prev sum, add the new number */
  ma->sum = ma->sum - ma->array[ma->pos] + new_count;

  /* Assign the nextNum to the position in the array */
  ma->array[ma->pos] = new_count;

  /* Bump to next position in array. */
  ma->pos++;
  if (ma->pos >= len)
     ma->pos = 0;

  /* Return the average */
  return ma->sum / len;
}

/* 
 * Read dongle state bits. Blocks on 1ms intervals. Returns following bitfield definitions.
 *    abort_bit = 0x1
 *    empty_bit = 0x2 
 *    input0_bit = 0x8 
 *    input2_bit = 0x10 
 *    input3_bit = 0x20
 * All bits are active high: 1=true 0=false
 */
enum EMC_RESULT rtstepper_state_query(struct emc_session *ps)
{
   struct step_query query_response;
   enum EMC_RESULT stat;
   int len;
   static int good_query = 0;

   /* Clear dongle state bits. */
   ps->state_bits &= ~(RTSTEPPER_STEP_STATE_INPUT0_BIT | RTSTEPPER_STEP_STATE_INPUT1_BIT | RTSTEPPER_STEP_STATE_INPUT2_BIT |
                       RTSTEPPER_STEP_STATE_ABORT_BIT | RTSTEPPER_STEP_STATE_EMPTY_BIT | RTSTEPPER_STEP_STATE_SYNC_START_BIT |
                       RTSTEPPER_STEP_STATE_INPUT3_BIT);

   if (ps->fd_table.hd == NULL)
   {
      stat = RTSTEPPER_R_REQ_ERROR;
      goto bugout;
   }

   len = libusb_control_transfer(ps->fd_table.hd, LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_INTERFACE,      /* bmRequestType */
                         STEP_QUERY,    /* bRequest */
                         0x0,   /* wValue */
                         DONGLE_INTERFACE, /* wIndex */
                         (unsigned char *)&query_response, sizeof(query_response), LIBUSB_CONTROL_REQ_TIMEOUT);

   if (len != sizeof(query_response))
   {
      if (len == LIBUSB_ERROR_NO_DEVICE)
      {
         stat = RTSTEPPER_R_DEVICE_UNAVAILABLE;
      }
      else
      {
         if (query_msg_cnt++ < 5)
            BUG("invalid usb query_response len=%d good_query_cnt=%d: %s\n", len, good_query, libusb_error_name(len));
         stat = RTSTEPPER_R_IO_ERROR;
      }
      goto bugout;
   }
   else
      good_query++;

   ps->state_bits |= query_response.state_bits._word;

   ps->icount_period = query_response.icount_period;
   ps->icount_period_avg = icount_moving_avg(ps, &ps->icount_ma, ps->icount_period);
   if (ps->current_io_type == RTSTEPPER_IO_TYPE_SPINDLE_SYNC)
   {
      if (ps->icount_period > ps->icount_period_max)
          ps->icount_period_max = ps->icount_period;
      if (ps->icount_period < ps->icount_period_min)
          ps->icount_period_min = ps->icount_period;
   }

   stat = EMC_R_OK;

 bugout:
   return stat;
}       /* rtstepper_state_query() */

enum EMC_RESULT rtstepper_state_adc_query(struct emc_session *ps)
{
   struct step_adc_query query_response;
   enum EMC_RESULT stat;
   int len;
   static int good_query = 0;

   /* Clear dongle state bits. */
   ps->state_bits &= ~(RTSTEPPER_STEP_STATE_INPUT0_BIT | RTSTEPPER_STEP_STATE_INPUT1_BIT | RTSTEPPER_STEP_STATE_INPUT2_BIT |
                       RTSTEPPER_STEP_STATE_ABORT_BIT | RTSTEPPER_STEP_STATE_EMPTY_BIT | RTSTEPPER_STEP_STATE_SYNC_START_BIT |
                       RTSTEPPER_STEP_STATE_INPUT3_BIT);

   if (ps->fd_table.hd == NULL)
   {
      stat = RTSTEPPER_R_REQ_ERROR;
      goto bugout;
   }

   len = libusb_control_transfer(ps->fd_table.hd, LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_INTERFACE,      /* bmRequestType */
                         STEP_ADC_QUERY,    /* bRequest */
                         0x0,   /* wValue */
                         DONGLE_INTERFACE, /* wIndex */
                         (unsigned char *)&query_response, sizeof(query_response), LIBUSB_CONTROL_REQ_TIMEOUT);

   if (len != sizeof(query_response))
   {
      if (len == LIBUSB_ERROR_NO_DEVICE)
      {
         stat = RTSTEPPER_R_DEVICE_UNAVAILABLE;
      }
      else
      {
         if (query_msg_cnt++ < 5)
            BUG("invalid usb query_response len=%d good_query_cnt=%d: %s\n", len, good_query, libusb_error_name(len));
         stat = RTSTEPPER_R_IO_ERROR;
      }
      goto bugout;
   }
   else
      good_query++;

   ps->state_bits |= query_response.state_bits._word;

   ps->icount_period = query_response.icount_period;
   ps->icount_period_avg = icount_moving_avg(ps, &ps->icount_ma, ps->icount_period);
   if (ps->current_io_type == RTSTEPPER_IO_TYPE_SPINDLE_SYNC)
   {
      if (ps->icount_period > ps->icount_period_max)
          ps->icount_period_max = ps->icount_period;
      if (ps->icount_period < ps->icount_period_min)
          ps->icount_period_min = ps->icount_period;
   }

   /* Save any analog to digital conversion (ADC). */
   ps->input1_adc = query_response.adc.input1;
   ps->input2_adc = query_response.adc.input2;
   ps->input3_adc = query_response.adc.input3;

   stat = EMC_R_OK;

 bugout:
   return stat;
}   /* rtstepper_state_adc_query() */

/* Clear runtime stats. Assumes machine is in an idle state. */
enum EMC_RESULT rtstepper_clear_stats(struct emc_session *ps)
{
   if (ps == NULL)
      return RTSTEPPER_R_IO_ERROR;

   ps->icount_period_max = 0;
   ps->icount_period_min = RTSTEPPER_ICOUNT_MAX;
   return EMC_R_OK;
}

enum EMC_RESULT rtstepper_open(struct emc_session *ps)
{
   struct step_elements elements;
   enum EMC_RESULT stat = RTSTEPPER_R_IO_ERROR;
   int i, len;

   DBG("rtstepper_open() ps=%p\n", ps);

   /* Set step clock default here so Trajectory Planner will work with no dongle. */
   ps->step_clock = RTSTEPPER_STEP_CLOCK_PIC;  /* dongle step clock in hz */

   if (ps == NULL)
      return RTSTEPPER_R_IO_ERROR;

   step_msg_cnt = query_msg_cnt = 0;

   ps->old_state_bits = 0;
   for (i=0; i < ps->axes; i++)
   {
      ps->axis[i].clk_tail = 0;
      ps->axis[i].direction = 0;
   }

   /* Open first usb device or usb device matching specified serial number. */
   if ((stat = open_device(&ps->fd_table, ps->serial_num)) != EMC_R_OK)
   {
      if (ps->serial_num[0])
         MSG("unable to find rtstepper dongle serial number: %s\n", ps->serial_num);
      else
         MSG("unable to find rtstepper dongle\n");
      goto bugout;
   }

   /* Clear any outstanding Abort and step count. */
   memset(&elements, 0, sizeof(elements));
   len = libusb_control_transfer(ps->fd_table.hd, LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_INTERFACE,      /* bmRequestType */
                         STEP_SET,      /* bRequest */
                         0x0,   /* wValue */
                         DONGLE_INTERFACE, /* wIndex */
                         (unsigned char *) &elements, sizeof(elements), LIBUSB_CONTROL_REQ_TIMEOUT);

   if (len != sizeof(elements))
   {
      BUG("unable to initialize dongle: %s\n", libusb_error_name(len));
      close_device(&ps->fd_table);
      goto bugout;
   }

   /* Clear any outstanding io request in the queue. */
   xfr_cancel(ps);

   rtstepper_clear_stats(ps);

   /* If ATmega32U4 dongle configure INPUT/OUTPUT pins, otherwise they tri-state on reset. */
   if (ps->fd_table.board_rev > RTSTEPPER_BRD_e)
   {
      dsp_input_mode(ps, INPUT1_NUM, ini_getint(ps->ini_file, "TASK", "INPUT1_MODE", IMP_DIGITAL, 1));
      dsp_input_mode(ps, INPUT2_NUM, ini_getint(ps->ini_file, "TASK", "INPUT2_MODE", IMP_DIGITAL, 1));
      dsp_input_mode(ps, INPUT3_NUM, ini_getint(ps->ini_file, "TASK", "INPUT3_MODE", IMP_DIGITAL, 1));
      dsp_output_mode(ps, OUTPUT0_NUM, ini_getint(ps->ini_file, "TASK", "OUTPUT0_MODE", OMP_DIGITAL, 1));
      dsp_output_mode(ps, OUTPUT1_NUM, ini_getint(ps->ini_file, "TASK", "OUTPUT1_MODE", OMP_DIGITAL, 1));
      ps->step_clock = RTSTEPPER_STEP_CLOCK_AVR;  /* dongle step clock in hz */
   }

   stat = EMC_R_OK;

 bugout:
   return stat;
}       /* rtstepper_open() */

enum EMC_RESULT rtstepper_close(struct emc_session *ps)
{
   enum EMC_RESULT stat;

   DBG("rtstepper_close() ps=%p\n", ps);

   if (ps == NULL)
      return RTSTEPPER_R_IO_ERROR;

   stat = close_device(&ps->fd_table);

   return stat;
}       /* rtstepper_close() */

/********************************************************************************************
 * Dongle standalone test code. Uses synchronous USB IO. Called by rt-test.py.
 ********************************************************************************************/

#define TBUG(args...) fprintf(stderr, __FILE__ " " STRINGIZE(__LINE__) ": " args)

static int bulk_write(struct rtstepper_file_descriptor *pfd, const void *buf, int size, int msec)
{
   int len=-EIO, r;

   if (pfd->hd == NULL)
   {
      TBUG("invalid bulk_write state\n");
      goto bugout;
   }

   r = libusb_bulk_transfer(pfd->hd, DONGLE_OUT_EP, (unsigned char *)buf, size, &len, msec);

   if (r == LIBUSB_ERROR_TIMEOUT)
   {
      len = -ETIMEDOUT;
      goto bugout;
   }

   if (r != 0)
   {
      TBUG("bulk_write failed len=%d: %s\n", len, libusb_error_name(r));
      goto bugout;
   }

bugout:
   return len;
} /* musb_write() */

static int open_test_device(struct rtstepper_file_descriptor *pfd, const char *sn)
{
   int stat = 1;
   int i, n;
   
   libusb_init(&pfd->ctx);
   libusb_set_debug(pfd->ctx, 3);
   n = libusb_get_device_list(pfd->ctx, &pfd->list_all);

   /* Look for rtstepper device(s). */
   for (i=0; i < n; i++)
   {
      if (is_rt(pfd->list_all[i], sn))
      {
         pfd->dev = pfd->list_all[i];
         if (claim_interface(pfd))
            goto bugout;

         stat = 0;
         break;
      }
   }

bugout:
   return stat;
}       /* open_test_device() */

enum EMC_RESULT rtstepper_test(const char *snum)
{
   struct rtstepper_file_descriptor fd_table;        /* usb file descriptors */
   struct step_elements elements;
   struct step_query query_response;
   char buf[65536];             /* for testing pick multiple 64-byte writes otherwise there could be a stall */
   unsigned char toggle;
   int i, ret = RTSTEPPER_R_DEVICE_UNAVAILABLE, len, tmo;

   fd_table.hd = NULL;

   /* Open first usb device or usb device matching specified serial number. */
   if (open_test_device(&fd_table, snum) != 0)
   {
      if (snum[0])
         TBUG("unable to find rtstepper dongle serial number: %s\n", snum);
      else
         TBUG("unable to find rtstepper dongle\n");
      goto bugout;
   }

   /* Clear device state bits and running step count. */
   memset(&elements, 0, sizeof(elements));
   len = libusb_control_transfer(fd_table.hd, LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_INTERFACE,      /* bmRequestType */
                         STEP_SET,      /* bRequest */
                         0x0,   /* wValue */
                         DONGLE_INTERFACE, /* wIndex */
                         (unsigned char *) &elements, sizeof(elements), LIBUSB_CONTROL_REQ_TIMEOUT);

   if (len != sizeof(elements))
   {
      TBUG("unable to initialize dongle len=%d: %s\n", len, libusb_error_name(len));
      goto bugout;
   }

   sleep(1);  /* Added delay for new faster PCs. DES 8-22-2015 */

   /* Verify state. */
   len = libusb_control_transfer(fd_table.hd, LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_INTERFACE,      /* bmRequestType */
                         STEP_QUERY,    /* bRequest */
                         0x0,   /* wValue */
                         DONGLE_INTERFACE, /* wIndex */
                         (unsigned char *)&query_response, sizeof(query_response), LIBUSB_CONTROL_REQ_TIMEOUT);
   if (len != sizeof(query_response))
   {
      TBUG("invalid query_response len=%d: %s\n", len, libusb_error_name(len));
      goto bugout;
   }

   fprintf(stdout, "query_response (Begin) len=%d period=%d state=%x steps=%d\n", len,
           query_response.icount_period, query_response.state_bits._word, query_response.step);

   toggle = 0x55;
   for (i = 0; i < sizeof(buf); i++)
   {
      buf[i] = toggle;
      toggle = ~toggle;
   }

   if ((len = bulk_write(&fd_table, buf, sizeof(buf), LIBUSB_TIMEOUT)) < 0)
   {
      TBUG("unable to write data len=%d snum=%s\n", len, snum);
      goto bugout;
   }
   fprintf(stdout, "wrote %d bytes\n", len);

   /* Wait for write to finish. */
   tmo = (double) sizeof(buf) * 0.000021333;    /* timeout in seconds = steps * period */
   tmo += 1;    /* plus 1 second */
   sleep(tmo);

   len = libusb_control_transfer(fd_table.hd, LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_INTERFACE,      /* bmRequestType */
                         STEP_QUERY,    /* bRequest */
                         0x0,   /* wValue */
                         DONGLE_INTERFACE, /* wIndex */
                         (unsigned char *)&query_response, sizeof(query_response), LIBUSB_CONTROL_REQ_TIMEOUT);
   if (len != sizeof(query_response))
   {
      TBUG("invalid query_response len=%d: %s\n", len, libusb_error_name(len));
      goto bugout;
   }

   fprintf(stdout, "query_response (End) len=%d period=%d state=%x steps=%d\n", len,
           query_response.icount_period, query_response.state_bits._word, query_response.step);

   release_interface(&fd_table);

//   if (query_response.step != sizeof(buf) || query_response.state_bits._word & STEP_STATE_STALL_BIT)
   if (query_response.step != sizeof(buf))
   {
      fprintf(stderr, "USB test failed.\n");
      fprintf(stderr, "  Suggestions in order:\n");
      fprintf(stderr, "     1. Remove any hubs, plug directly into PC.\n");
      fprintf(stderr, "     2. Try a certified HIGH-SPEED or USB 2.0 cable.\n");
      fprintf(stderr, "     3. Try a different USB port directly into PC.\n");
      fprintf(stderr, "     4. Kill all extraneous resource intensive Applications.\n");
      fprintf(stderr, "     5. Try running in a TTY terminal without Gnome or KDE.\n");
      fprintf(stderr, "     6. Try a different PC. Your USB subsytem may not support rt-stepper\n");
      ret = 1;
   }
   else
   {
      fprintf(stdout, "USB test passed.\n");
      ret = EMC_R_OK;
   }

 bugout:
   return ret;
}   /* rtstepper_test() */
