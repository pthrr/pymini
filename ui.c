/*****************************************************************************\

  ui.c - user interface support for rtstepperemc

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

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <pthread.h>
#include <math.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include "emc.h"
#include "ini.h"
#include "bug.h"

/* Following GUI_EVENT must match python GuiEvent. */
enum GUI_EVENT
{
   GUI_EVENT_MECH_IDLE = 1,
   GUI_EVENT_LOG_MSG = 2,
   GUI_EVENT_MECH_DEFAULT = 3,
   GUI_EVENT_MECH_POSITION = 4,
   GUI_EVENT_MECH_ESTOP = 5,
   GUI_EVENT_MECH_PAUSED = 6,
};

struct emcpose_py
{
   double x;
   double y;
   double z;
   double a;
   double b;
   double c;
   double u;
   double v;
   double w;
};

struct post_position_py
{
   int id;
   struct emcpose_py pos;
};

char USER_HOME_DIR[LINELEN];

static logger_cb_t _logger_cb = NULL;
static post_event_cb_t _post_event_cb = NULL;
static post_position_cb_t _post_position_cb = NULL;
static plugin_cb_t _plugin_cb = NULL;

static pthread_mutex_t ui_mutex = PTHREAD_MUTEX_INITIALIZER;

struct emc_session session;

/* Axis:                                    1    2    3    4     5     6     7     8      9  */
static const int _map_axes_mask[] = { 0x0, 0x1, 0x3, 0x7, 0xf, 0x1f, 0x3f, 0x7f, 0xff, 0x1ff };
static const char *_map_axis_coordinate_default[] = {"X", "Y", "Z", "A", "B", "C", "U", "V", "W"};

static enum EmcAxisType _map_axis_type(const char *type)
{
   if (strncasecmp(type, "linear", 6) == 0)
      return EMC_AXIS_LINEAR;
   if (strncasecmp(type, "angular", 7) == 0)
      return EMC_AXIS_ANGULAR;
   return EMC_AXIS_LINEAR;
}

static double _map_linear_units(const char *units)
{
   if (strncasecmp(units, "mm", 2) == 0)
      return 1.0;
   if (strncasecmp(units, "metric", 6) == 0)
      return 1.0;
   if (strncasecmp(units, "in", 2) == 0)
      return 1 / 25.4;
   if (strncasecmp(units, "inch", 4) == 0)
      return 1 / 25.4;
   if (strncasecmp(units, "imperial", 8) == 0)
      return 1 / 25.4;
   return 0;
}

static double _map_angular_units(const char *units)
{
   if (strncasecmp(units, "deg", 3) == 0)
      return 1.0;
   if (strncasecmp(units, "degree", 6) == 0)
      return 1.0;
   if (strncasecmp(units, "grad", 4) == 0)
      return 0.9;
   if (strncasecmp(units, "gon", 3) == 0)
      return 0.9;
   if (strncasecmp(units, "rad", 3) == 0)
      return M_PI / 180;
   if (strncasecmp(units, "radian", 6) == 0)
      return M_PI / 180;
   return 0;
}

static int _map_axis_coordinate(const char *coordinate)
{
   if (strncasecmp(coordinate, "x", 1) == 0)
      return EMC_AXIS_X;
   if (strncasecmp(coordinate, "y", 1) == 0)
      return EMC_AXIS_Y;
   if (strncasecmp(coordinate, "z", 1) == 0)
      return EMC_AXIS_Z;
   if (strncasecmp(coordinate, "a", 1) == 0)
      return EMC_AXIS_A;
   if (strncasecmp(coordinate, "b", 1) == 0)
      return EMC_AXIS_B;
   if (strncasecmp(coordinate, "c", 1) == 0)
      return EMC_AXIS_C;
   if (strncasecmp(coordinate, "u", 1) == 0)
      return EMC_AXIS_U;
   if (strncasecmp(coordinate, "v", 1) == 0)
      return EMC_AXIS_V;
   if (strncasecmp(coordinate, "w", 1) == 0)
      return EMC_AXIS_W;
   return EMC_AXIS_X;
}

static enum EMC_RESULT _load_tool_table(const char *filename, struct CANON_TOOL_TABLE toolTable[])
{
   int i, pocket;
   char path[LINELEN];
   char section[32];

   snprintf(path, sizeof(path), "%s/%s", USER_HOME_DIR, filename);

   /* Clear tool table, default is zero offsets. */
   for (i = 0; i < CANON_POCKETS_MAX; i++)
   {
      toolTable[i].toolno = -1;
      ZERO_EMC_POSE(toolTable[i].offset);
      toolTable[i].diameter = 0.0;
      toolTable[i].frontangle = 0.0;
      toolTable[i].backangle = 0.0;
      toolTable[i].orientation = 0;
   }

   /* Load tool table. Ignore pocket number 0. */
   for (i=1; i < CANON_POCKETS_MAX; i++)
   {
      sprintf(section, "T%d", i);

      pocket = ini_getint(path, section, "POCKET", -1, 0);      
      if (pocket < 1 || pocket >= CANON_POCKETS_MAX)
          continue;  /* skip */

      toolTable[pocket].toolno = i;

      toolTable[pocket].offset.tran.x = ini_getfloat(path, section, "X_OFFSET", 0.0, 0);
      toolTable[pocket].offset.tran.y = ini_getfloat(path, section, "Y_OFFSET", 0.0, 0);
      toolTable[pocket].offset.tran.z = ini_getfloat(path, section, "Z_OFFSET", 0.0, 0);
      toolTable[pocket].offset.a = ini_getfloat(path, section, "A_OFFSET", 0.0, 0);
      toolTable[pocket].offset.b = ini_getfloat(path, section, "B_OFFSET", 0.0, 0);
      toolTable[pocket].offset.c = ini_getfloat(path, section, "C_OFFSET", 0.0, 0);
      toolTable[pocket].offset.u = ini_getfloat(path, section, "U_OFFSET", 0.0, 0);
      toolTable[pocket].offset.v = ini_getfloat(path, section, "V_OFFSET", 0.0, 0);
      toolTable[pocket].offset.w = ini_getfloat(path, section, "W_OFFSET", 0.0, 0);
      toolTable[pocket].diameter = ini_getfloat(path, section, "DIAMETER", 0.0, 0);

      /*  Lathe specific parameters. */
      toolTable[pocket].frontangle = ini_getfloat(path, section, "FRONT_ANGLE", 0.0, 0);
      toolTable[pocket].backangle = ini_getfloat(path, section, "BACK_ANGLE", 0.0, 0);
      toolTable[pocket].orientation = ini_getint(path, section, "ORIENTATION", 0, 0);
   }
   
   return EMC_R_OK;
}  /* _load_tool_table() */

static enum EMC_RESULT _load_axis(struct emc_session *ps, int axis, int verbose)
{
   char inistring[LINELEN];
   char section[32];

   sprintf(section, "AXIS_%d", axis);

   // set axis type
   ps->axis[axis].type = _map_axis_type(ini_get(ps->ini_file, section, "TYPE", inistring, sizeof(inistring), "linear", verbose));
   // set backlash
   ps->axis[axis].backlash = ini_getfloat(ps->ini_file, section, "BACKLASH", 0.0, verbose);
   // set min position limit
   ps->axis[axis].min_pos_limit = ini_getfloat(ps->ini_file, section, "MIN_LIMIT", -1e99, verbose);
   // set max position limit
   ps->axis[axis].max_pos_limit = ini_getfloat(ps->ini_file, section, "MAX_LIMIT", 1e99, verbose);
   // set following error limit (at max speed)
   ps->axis[axis].max_ferror = ini_getfloat(ps->ini_file, section, "FERROR", 1.0, verbose);
   // do MIN_FERROR, if it's there. If not, use value of maxFerror above
   ps->axis[axis].min_ferror = ini_getfloat(ps->ini_file, section, "MIN_FERROR", ps->axis[axis].max_ferror, verbose);
   ps->axis[axis].home = ini_getfloat(ps->ini_file, section, "HOME", 0.0, verbose);
   // set maximum velocity
   ps->axis[axis].max_velocity = ini_getfloat(ps->ini_file, section, "MAX_VELOCITY", DEFAULT_AXIS_MAX_VELOCITY, verbose);
   // set max acceleration
   ps->axis[axis].max_acceleration = ini_getfloat(ps->ini_file, section, "MAX_ACCELERATION", DEFAULT_AXIS_MAX_ACCELERATION, verbose);
   // set input scale
   ps->axis[axis].steps_per_unit = ini_getfloat(ps->ini_file, section, "INPUT_SCALE", 0.0, verbose);
   // set step pin
   ps->axis[axis].step_pin = ini_getint(ps->ini_file, section, "STEP_PIN", 0, verbose);
   // set direction pin
   ps->axis[axis].direction_pin = ini_getint(ps->ini_file, section, "DIRECTION_PIN", 0, verbose);
   // set step pen polarity
   ps->axis[axis].step_active_high = ini_getint(ps->ini_file, section, "STEP_ACTIVE_HIGH", 0, verbose);
   // set direction pin polarity
   ps->axis[axis].direction_active_high = ini_getint(ps->ini_file, section, "DIRECTION_ACTIVE_HIGH", 0, verbose);
   // set axis to coordinate map
   ps->axis[axis].coordinate_map = _map_axis_coordinate(ini_get(ps->ini_file, section, "COORDINATE", inistring, sizeof(inistring), _map_axis_coordinate_default[axis], 0));

   return EMC_R_OK;
} /* _load_axis() */

static void _emcpose2py(struct emcpose_py *pospy, EmcPose pos)
{
   pospy->x = pos.tran.x;
   pospy->y = pos.tran.y;
   pospy->z = pos.tran.z;
   pospy->a = pos.a;
   pospy->b = pos.b;
   pospy->c = pos.c;
   pospy->u = pos.u;
   pospy->v = pos.v;
   pospy->w = pos.w;
}  /* _emcpose2py() */

static void _py2emcpose(EmcPose *pos, struct emcpose_py *pospy)
{
   pos->tran.x = pospy->x;
   pos->tran.y = pospy->y;
   pos->tran.z = pospy->z;
   pos->a = pospy->a;
   pos->b = pospy->b;
   pos->c = pospy->c;
   pos->u = pospy->u;
   pos->v = pospy->v;
   pos->w = pospy->w;
}  /* _emcpose2py() */

void esleep(double seconds)
{
   if (seconds <= 0.0)
      return;

#if (defined(__WIN32__) || defined(_WINDOWS))
   SleepEx(((unsigned long) (seconds * 1000)), FALSE);
#else
   struct timespec rqtp;
   rqtp.tv_sec = seconds;
   rqtp.tv_nsec = (seconds - rqtp.tv_sec) * 1E9;
   if (nanosleep(&rqtp, NULL) < 0)
   {
      if (errno != EINTR)
      {
         BUG("nanosleep({tv_sec=%d,tv_nsec=%ld}) error (errno=%d) %s\n", (int) rqtp.tv_sec, rqtp.tv_nsec, errno, strerror(errno));
      }
   }
#endif
   return;
}       /* esleep() */

enum EMC_RESULT emc_logger_cb(const char *fmt, ...)
{
   va_list args;
   char tmp[512];

   pthread_mutex_lock(&ui_mutex);

   va_start(args, fmt);

   vsnprintf(tmp, sizeof(tmp), fmt, args);
   tmp[sizeof(tmp) - 1] = 0;    /* force zero termination */

   if (_logger_cb)
      (_logger_cb) (tmp);  /* make direct call to python logger */
   else
      fputs(tmp, stdout);

   va_end(args);

   pthread_mutex_unlock(&ui_mutex);
   return EMC_R_OK;
} /* emc_logger_cb() */

#if 0
enum EMC_RESULT emc_post_position_cb_old(EmcPose pos)
{
   int i=0, size=0;
   char value[16 * EMC_MAX_AXIS];
   char key[4 * EMC_MAX_AXIS];
   char *pkey[EMC_MAX_AXIS + 1];
   char *pvalue[EMC_MAX_AXIS + 1];
   
   pvalue[i++] = value;
   size += (snprintf(value+size, sizeof(value)-size, "%07.3f", pos.tran.x) + 1);
   pvalue[i++] = value+size;
   size += (snprintf(value+size, sizeof(value)-size, "%07.3f", pos.tran.y) + 1);
   pvalue[i++] = value+size;
   size += (snprintf(value+size, sizeof(value)-size, "%07.3f", pos.tran.z) + 1);
   pvalue[i++] = value+size;
   size += (snprintf(value+size, sizeof(value)-size, "%07.3f", pos.a) + 1);
   pvalue[i++] = value+size;
   size += (snprintf(value+size, sizeof(value)-size, "%07.3f", pos.b) + 1);
   pvalue[i++] = value+size;
   size += (snprintf(value+size, sizeof(value)-size, "%07.3f", pos.c) + 1);
   pvalue[i++] = value+size;
   size += (snprintf(value+size, sizeof(value)-size, "%07.3f", pos.u) + 1);
   pvalue[i++] = value+size;
   size += (snprintf(value+size, sizeof(value)-size, "%07.3f", pos.v) + 1);
   pvalue[i++] = value+size;
   size += (snprintf(value+size, sizeof(value)-size, "%07.3f", pos.w) + 1);

   /* Force zero termination. */
   value[sizeof(value) - 1] = 0;
   pvalue[i] = NULL;

   i = size = 0;
   pkey[i++] = key;
   size += (snprintf(key+size, sizeof(key)-size, "x") + 1);
   pkey[i++] = key+size;
   size += (snprintf(key+size, sizeof(key)-size, "y") + 1);
   pkey[i++] = key+size;
   size += (snprintf(key+size, sizeof(key)-size, "z") + 1);
   pkey[i++] = key+size;
   size += (snprintf(key+size, sizeof(key)-size, "a") + 1);
   pkey[i++] = key+size;
   size += (snprintf(key+size, sizeof(key)-size, "b") + 1);
   pkey[i++] = key+size;
   size += (snprintf(key+size, sizeof(key)-size, "c") + 1);
   pkey[i++] = key+size;
   size += (snprintf(key+size, sizeof(key)-size, "u") + 1);
   pkey[i++] = key+size;
   size += (snprintf(key+size, sizeof(key)-size, "v") + 1);
   pkey[i++] = key+size;
   size += (snprintf(key+size, sizeof(key)-size, "w") + 1);

   /* Force zero termination. */
   key[sizeof(key) - 1] = 0;
   pkey[i] = NULL;

   if (_gui_event_cb)
      (_gui_event_cb) (GUI_EVENT_MECH_POSITION, i, pkey, pvalue);

   return EMC_R_OK;
}
#endif

enum EMC_RESULT emc_position_post_cb(int id, EmcPose pos)
{
   struct post_position_py post;

   DBG("emc_position_post_cb() line_num=%d x=%0.5f y=%0.5f z_pos=%0.5f\n", id, pos.tran.x, pos.tran.y, pos.tran.z);
   post.id = id;
   _emcpose2py(&post.pos, pos);

   if (_post_position_cb)
      (_post_position_cb) (GUI_EVENT_MECH_POSITION, &post);  /* post message to gui queue */

   return EMC_R_OK;
}

enum EMC_RESULT emc_estop_post_cb(struct emc_session *ps)
{
   ps->state_bits |= EMC_STATE_ESTOP_BIT;

   DBG("emc_estop_post_cb()\n");
   if (_post_event_cb)
      (_post_event_cb) (GUI_EVENT_MECH_ESTOP);  /* post message to gui queue */
   return EMC_R_OK;
}

enum EMC_RESULT emc_paused_post_cb(struct emc_session *ps)
{
   ps->state_bits |= EMC_STATE_PAUSED_BIT;

   DBG("emc_paused_post_cb()\n");
   if (_post_event_cb)
      (_post_event_cb) (GUI_EVENT_MECH_PAUSED);  /* post message to gui queue */
   return EMC_R_OK;
}

enum EMC_RESULT emc_plugin_cb(int mcode, double p_number, double q_number)
{
   DBG("emc_plugin_cb() M%d\n", mcode);

   if (_plugin_cb)
      return (_plugin_cb) (mcode, p_number, q_number);   /* make direct call to python plugin */

   return EMC_R_OK;
}
DLL_EXPORT enum EMC_RESULT emc_ui_logger_register_cb(logger_cb_t fp)
{
   _logger_cb = fp;
   return EMC_R_OK;
}

DLL_EXPORT enum EMC_RESULT emc_ui_gui_event_register_cb(post_event_cb_t fp)
{
   _post_event_cb = fp;
   return EMC_R_OK;
}

DLL_EXPORT enum EMC_RESULT emc_ui_position_register_cb(post_position_cb_t fp)
{
   _post_position_cb = fp;
   return EMC_R_OK;
}

DLL_EXPORT enum EMC_RESULT emc_ui_plugin_register_cb(plugin_cb_t fp)
{
   _plugin_cb = fp;
   return EMC_R_OK;
}

DLL_EXPORT enum EMC_RESULT emc_ui_state_get(void *hd, unsigned long *stat)
{
   struct emc_session *ps = (struct emc_session *)hd;
   *stat = ps->state_bits;
   return EMC_R_OK;
}

DLL_EXPORT enum EMC_RESULT emc_ui_version_get(const char **ver)
{
   *ver = PACKAGE_VERSION;
   return EMC_R_OK;
}

DLL_EXPORT enum EMC_RESULT emc_ui_estop(void *hd)
{
   struct emc_session *ps = (struct emc_session *)hd;
   DBG("emc_ui_estop() called\n");
   return dsp_estop(ps);
}       /* emc_ui_estop() */

DLL_EXPORT enum EMC_RESULT emc_ui_estop_reset(void *hd)
{
   struct emc_session *ps = (struct emc_session *)hd;
   DBG("emc_ui_estop_reset() called\n");
   return dsp_estop_reset(ps);
}       /* emc_ui_send_estop_reset() */

DLL_EXPORT enum EMC_RESULT emc_ui_home(void *hd)
{
   struct emc_session *ps = (struct emc_session *)hd;
   DBG("emc_ui_home() called\n");
   return dsp_home(ps);
}       /* emc_ui_send_home() */

DLL_EXPORT enum EMC_RESULT emc_ui_position_get(void *hd, struct emcpose_py *pospy)
{
   struct emc_session *ps = (struct emc_session *)hd;
   DBG("emc_ui_position_get() called x=%0.5f y=%0.5f z_pos=%0.5f\n", ps->position.tran.x, ps->position.tran.y, ps->position.tran.z);
   _emcpose2py(pospy, ps->position);
   return EMC_R_OK;
}

DLL_EXPORT enum EMC_RESULT emc_ui_position_set(void *hd, struct emcpose_py *pospy)
{
   struct emc_session *ps = (struct emc_session *)hd;
   EmcPose pos;
   _py2emcpose(&pos, pospy);
   return dsp_position_set(ps, pos);
}

DLL_EXPORT enum EMC_RESULT emc_ui_io_done_wait(void *hd)
{
   struct emc_session *ps = (struct emc_session *)hd;
   return dsp_io_done_wait(ps);
}

DLL_EXPORT enum EMC_RESULT emc_ui_mdi_cmd(void *hd, const char *mdi)
{
   struct emc_session *ps = (struct emc_session *)hd;
   return dsp_mdi(ps, mdi);
}       /* emc_ui_mdi_cmd() */

DLL_EXPORT enum EMC_RESULT emc_ui_auto_cmd(void *hd, const char *gcode_file)
{
   struct emc_session *ps = (struct emc_session *)hd;
   return dsp_auto(ps, gcode_file);
}       /* emc_ui_auto_cmd() */

DLL_EXPORT enum EMC_RESULT emc_ui_auto_cancel_set(void *hd)
{
   struct emc_session *ps = (struct emc_session *)hd;
   DBG("emc_ui_auto_cancel_set() called\n");
   return dsp_auto_cancel_set(ps);
}       /* emc_ui_verify_cancel_set() */

DLL_EXPORT enum EMC_RESULT emc_ui_auto_cancel_clear(void *hd)
{
   struct emc_session *ps = (struct emc_session *)hd;
   DBG("emc_ui_auto_cancel_clear() called\n");
   return dsp_auto_cancel_clear(ps);
}       /* emc_ui_auto_cancel_clear() */

DLL_EXPORT enum EMC_RESULT emc_ui_verify_cmd(void *hd, const char *gcode_file)
{
   struct emc_session *ps = (struct emc_session *)hd;
   return dsp_verify(ps, gcode_file);
}       /* emc_ui_verify_cmd() */

DLL_EXPORT enum EMC_RESULT emc_ui_verify_cancel_set(void *hd)
{
   struct emc_session *ps = (struct emc_session *)hd;
   DBG("emc_ui_verify_cancel_set() called\n");
   return dsp_verify_cancel_set(ps);
}       /* emc_ui_verify_cancel_set() */

DLL_EXPORT enum EMC_RESULT emc_ui_verify_cancel_clear(void *hd)
{
   struct emc_session *ps = (struct emc_session *)hd;
   DBG("emc_ui_verify_cancel_clear() called\n");
   return dsp_verify_cancel_clear(ps);
}       /* emc_ui_verify_cancel_clear() */

DLL_EXPORT enum EMC_RESULT emc_ui_paused_clear(void *hd)
{
   struct emc_session *ps = (struct emc_session *)hd;
   DBG("emc_ui_paused_clear() called\n");
   return dsp_paused_clear(ps);
}       /* emc_ui_paused_clear() */

DLL_EXPORT enum EMC_RESULT emc_ui_din_abort_enable(void *hd, int input_num)
{
   struct emc_session *ps = (struct emc_session *)hd;
   return dsp_din_abort_enable(ps, input_num);
}       /*  emc_ui_din_abort_enable() */

DLL_EXPORT enum EMC_RESULT emc_ui_din_abort_disable(void *hd, int input_num)
{
   struct emc_session *ps = (struct emc_session *)hd;
   return dsp_din_abort_disable(ps, input_num);
}       /*  emc_ui_din_abort_disable() */

DLL_EXPORT enum EMC_RESULT emc_ui_din_frequency_get(void *hd, int input_num, double *value)
{
   struct emc_session *ps = (struct emc_session *)hd;

   *value = 0.0;

   if (ps->input0_abort_enabled == 0 && ps->fd_table.board_rev > RTSTEPPER_BRD_c)
      if (ps->icount_period > 0)
         *value = (double)ps->step_clock / ps->icount_period;

   return EMC_R_OK;
}

DLL_EXPORT enum EMC_RESULT emc_ui_din_frequency_avg_get(void *hd, int input_num, double *value)
{
   struct emc_session *ps = (struct emc_session *)hd;

   *value = 0.0;

   if (ps->input0_abort_enabled == 0 && ps->fd_table.board_rev > RTSTEPPER_BRD_c)
      if (ps->icount_period_avg > 0)
         *value = (double)ps->step_clock / ps->icount_period_avg;

   return EMC_R_OK;
}

DLL_EXPORT enum EMC_RESULT emc_ui_din_frequency_max_get(void *hd, int input_num, double *value)
{
   struct emc_session *ps = (struct emc_session *)hd;

   *value = 0.0;

   if (ps->input0_abort_enabled == 0 && ps->fd_table.board_rev > RTSTEPPER_BRD_c && ps->icount_period_min != RTSTEPPER_ICOUNT_MAX)
      if (ps->icount_period_min > 0)
         *value = (double)ps->step_clock / ps->icount_period_min;

   return EMC_R_OK;
}

DLL_EXPORT enum EMC_RESULT emc_ui_din_frequency_min_get(void *hd, int input_num, double *value)
{
   struct emc_session *ps = (struct emc_session *)hd;

   *value = 0.0;

   if (ps->input0_abort_enabled == 0 && ps->fd_table.board_rev > RTSTEPPER_BRD_c)
      if (ps->icount_period_max > 0)
         *value = (double)ps->step_clock / ps->icount_period_max;

   return EMC_R_OK;
}

DLL_EXPORT enum EMC_RESULT emc_ui_dout_set(void *hd, int output_num)
{
   struct emc_session *ps = (struct emc_session *)hd;
   return dsp_dout_set(ps, output_num);
}

DLL_EXPORT enum EMC_RESULT emc_ui_dout_clear(void *hd, int output_num)
{
   struct emc_session *ps = (struct emc_session *)hd;
   return dsp_dout_clear(ps, output_num);
}

DLL_EXPORT enum EMC_RESULT emc_ui_output_mode(void *hd, int output_num, int param)
{
   struct emc_session *ps = (struct emc_session *)hd;
   return dsp_output_mode(ps, output_num, param);
}

DLL_EXPORT enum EMC_RESULT emc_ui_output_pwm(void *hd, int output_num, int param)
{
   struct emc_session *ps = (struct emc_session *)hd;
   return dsp_output_pwm(ps, output_num, param);
}

DLL_EXPORT enum EMC_RESULT emc_ui_input_mode(void *hd, int input_num, int param)
{
   struct emc_session *ps = (struct emc_session *)hd;
   return dsp_input_mode(ps, input_num, param);
}

DLL_EXPORT enum EMC_RESULT emc_ui_input_adc_get(void *hd, int input_num, int *value)
{
   struct emc_session *ps = (struct emc_session *)hd;

   *value = 0;
   if (ps->fd_table.board_rev > RTSTEPPER_BRD_e)
   {
      if (input_num == 1)
         *value = ps->input1_adc;
      else if (input_num == 2)
         *value = ps->input2_adc;
      else if (input_num == 3)
         *value = ps->input3_adc;
   }
   return EMC_R_OK;
}

DLL_EXPORT enum EMC_RESULT emc_ui_test(const char *snum)
{
   return rtstepper_test(snum);
}

DLL_EXPORT void *emc_ui_open(const char *home, const char *ini_file)
{
   struct emc_session *ret = NULL, *ps = &session;
   char inistring[LINELEN];
   int i, rstat;

   DBG("[%d] emc_ui_open() ini=%s\n", getpid(), ini_file);

   strncpy(USER_HOME_DIR, home, sizeof(USER_HOME_DIR));
   USER_HOME_DIR[sizeof(USER_HOME_DIR)-1] = 0;  /* force zero termination */

   strncpy(ps->ini_file, ini_file, sizeof(ps->ini_file));
   ps->ini_file[sizeof(ps->ini_file)-1] = 0;  /* force zero termination */

   ini_get(ini_file, "TASK", "SERIAL_NUMBER", ps->serial_num, sizeof(ps->serial_num), "", 1);

   ps->input0_abort_enabled = ini_getint(ini_file, "TASK", "INPUT0_ABORT", 0, 1);
   ps->input1_abort_enabled = ini_getint(ini_file, "TASK", "INPUT1_ABORT", 0, 1);
   ps->input2_abort_enabled = ini_getint(ini_file, "TASK", "INPUT2_ABORT", 0, 1);
   ps->input3_abort_enabled = ini_getint(ini_file, "TASK", "INPUT3_ABORT", 0, 0); // new for REV-3f

   ps->axes = ini_getint(ini_file, "TRAJ", "AXES", 4, 1);
   if (ps->axes > EMC_MAX_JOINTS)
   {
      BUG("Invalid ini file setting: axes=%d\n", ps->axes);
      ps->axes = 4;
   }
   ps->axes_mask = _map_axes_mask[ps->axes];

   ps->linearUnits = _map_linear_units(ini_get(ini_file, "TRAJ", "LINEAR_UNITS", inistring, sizeof(inistring), "inch", 1));
   ps->angularUnits = _map_angular_units(ini_get(ini_file, "TRAJ", "ANGULAR_UNITS", inistring, sizeof(inistring), "degree", 1));

   // by default, use AXIS limit
   ps->maxVelocity = ini_getfloat(ini_file, "TRAJ", "MAX_VELOCITY", 1e99, 1);
   // by default, use AXIS limit
   ps->maxAcceleration = ini_getfloat(ini_file, "TRAJ", "MAX_ACCELERATION", 1e99, 1);

   _load_tool_table(ini_get(ini_file, "EMC", "TOOL_TABLE", inistring, sizeof(inistring), "stepper.tbl", 1), ps->toolTable);

   /* Set defaults for all nine axis. */
   for (i=0; i < EMC_MAX_AXIS; i++)
      _load_axis(ps, i, (i < ps->axes) ? 1 : 0);

   INIT_LIST_HEAD(&ps->head.list);
   INIT_LIST_HEAD(&ps->ctrl_head.list);

   emc_position_post_cb(0, ps->position);

   rstat = rtstepper_open(ps);

   ps->cycle_time = 1 / ((double)ps->step_clock / 2);  /* waypoint period in seconds */
   ps->cycle_freq = 1 / ps->cycle_time;   /* waypoint in hz */

   if (dsp_open(ps) != EMC_R_OK || rstat != EMC_R_OK)
      emc_estop_post_cb(ps);

   ret = ps;

   return ret;  /* return an opaque handle */
}       /* emc_ui_open() */

DLL_EXPORT enum EMC_RESULT emc_ui_close(void *hd)
{
   struct emc_session *ps = (struct emc_session *)hd;
   DBG("[%d] emc_ui_close()\n", getpid());
   dsp_close(ps);
   rtstepper_close(ps);
   return EMC_R_OK;
}       /* emc_ui_close() */

static void emc_dll_init(void)
{
}       /* emc_dll_init() */

static void emc_dll_exit(void)
{
}       /* emc_dll_exit() */

#if (defined(__WIN32__) || defined(_WINDOWS))
BOOL WINAPI DllMain(HANDLE module, DWORD reason, LPVOID reserved)
{
   switch (reason)
   {
   case DLL_PROCESS_ATTACH:
      emc_dll_init();
      break;
   case DLL_PROCESS_DETACH:
      emc_dll_exit();
      break;
   default:
      break;
   }
   return TRUE;
}
#else
static void __attribute__ ((constructor)) _emc_init(void)
{
   emc_dll_init();
}

static void __attribute__ ((destructor)) _emc_exit(void)
{
   emc_dll_exit();
}
#endif
