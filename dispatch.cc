/*****************************************************************************\

  dispatch.cc - command dispatcher for rtstepperemc

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
#include <errno.h>
#include <time.h>
#include <string.h>
#include "emc.h"
#include "interpl.h"
#include "interp_return.h"
#include "rs274ngc_interp.h"    // the interpreter
#include "bug.h"

static Interp interp;
MSG_INTERP_LIST interp_list;    /* MSG Union, for interpreter */

static unsigned int sync_msg_cnt;

static void _interp_error(int retval, int line_number, EmcPose position)
{
   char buf[LINELEN];
   int i;

   /* Don't display MDI lines. */
   if (line_number > 0)
   {
      /* Update the display with gcode lines. Use small delay to let gui display first. */
      emc_position_post_cb(line_number, position); 
      esleep(0.2);
   }

   buf[0] = 0;
   interp.error_text(retval, buf, sizeof(buf));
   if (buf[0] != 0)
   {
      if (line_number > 0)
      {
         BUG("interpreter error: line %d %s\n", line_number, buf);
      }
      else
      {
         BUG("interpreter error: %s\n", buf);      
      }
   }

   /* Dump interpreter call stack. Debug only. */
   for (i = 0; i < 5; i++)
   {
      buf[0] = 0;
      interp.stack_name(i, buf, sizeof(buf));
      if (buf[0] == 0)
         break;
      DBG("  %s\n", buf);
   }
}       /* _interp_error() */

/* Run trajectory planner cycles until the move is complete. */
static void _run_tp(struct emc_session *ps, struct rtstepper_io_req *io)
{
   double sm_pos[EMC_MAX_AXIS];
   int cnt;
   unsigned int i;

   for (cnt=1; !tpIsDone(&ps->tp_queue); cnt++)
   {
      tpRunCycle(&ps->tp_queue);
#if 0
      if (cnt < 1500)
      {
         DBG("tpRunCycle() cnt=%d\n", cnt);
         tpPrint(&ps->tp_queue);
      }
#endif
      /* Extract position commands for leadscrew compensation. */
      update_tp_position(ps, tpGetPos(&ps->tp_queue));

      /* Calculate leadscrew compensation (backlash). */
      compute_screw_comp(ps);

      for (i=0; i < ps->axes; i++)
      {
         /* Apply backlash. */
         sm_pos[i] = ps->axis[i].pos_cmd + ps->axis[i].backlash_filt;

         /* Check soft position limit. */
         if (sm_pos[i] > 0.0)
            sm_pos[i] = (sm_pos[i] > ps->axis[i].max_pos_limit) ? ps->axis[i].max_pos_limit : sm_pos[i];
         if (sm_pos[i] < 0.0)
            sm_pos[i] = (sm_pos[i] < ps->axis[i].min_pos_limit) ? ps->axis[i].min_pos_limit : sm_pos[i];
      }

      //DBG("X vel_cmd=%0.9f, X bl_vel=%0.9f, X pos_cmd=%0.9f, X sm=%0.9f, X backlash=%0.9f\n", 
      //ps->axis[0].vel_cmd, ps->axis[0].backlash_vel, ps->axis[0].pos_cmd, sm_pos[0], ps->axis[0].backlash_filt);

      /* Encode step buffer. */
      rtstepper_encode(ps, io, sm_pos);
   }
}  /* _run_tp() */

/* Dispatch interpreter command. */
static enum EMC_RESULT _dsp_interp_cmd(struct emc_session *ps, emc_command_msg_t *cmd, int id)
{
   enum EMC_RESULT stat = EMC_R_ERROR;

   DBG("_dsp_interp_cmd() id=%d cmd=%s\n", id, lookup_message(cmd->msg.type));

   switch (cmd->msg.type)
   {
   case EMC_TRAJ_LINEAR_MOVE_TYPE:
      {
         emc_traj_linear_move_msg_t *p = (emc_traj_linear_move_msg_t *)cmd;
         struct rtstepper_io_req *io;
         double vel = p->vel;
         enum RTSTEPPER_IO_TYPE io_type = RTSTEPPER_IO_TYPE_SPINDLE_ASYNC;

         if (ps->sync_enabled)
         {
            vel = ps->sync_feed_per_sec;
            if (vel > p->ini_maxvel)
            {
               MSG("Warning synchronization velocity > max velocity (%0.5f > %0.5f) rpm=%0.5f", vel, p->ini_maxvel, (double)ps->step_clock / ps->icount_period * 60);
               vel = p->ini_maxvel;
            }
            io_type = RTSTEPPER_IO_TYPE_SPINDLE_SYNC;
         }

         tpSetId(&ps->tp_queue, id);
         tpSetVmax(&ps->tp_queue, vel);
         tpSetAmax(&ps->tp_queue, p->acc);
         tpAddLine(&ps->tp_queue, p->end);

         /* Allocate an io request transfer. */ 
         io = rtstepper_io_req_alloc(ps, id, io_type);       

         /* Run trajectory planner. */
         _run_tp(ps, io);

         DBG("L line=%d x_pos=%0.5f, x_master=%d y_pos=%0.5f, y_master=%d z_pos=%0.5f, z_master=%d, vel=%0.5f\n", id, 
         p->end.tran.x, ps->axis[EMC_AXIS_X].master_index, 
         p->end.tran.y, ps->axis[EMC_AXIS_Y].master_index, 
         p->end.tran.z, ps->axis[EMC_AXIS_Z].master_index, vel);

         /* Dispatch step buffer package to IO system. */
         if (rtstepper_xfr_start(ps, io, tpGetPos(&ps->tp_queue)) != EMC_R_OK)
            goto bugout;
        
         stat = EMC_R_OK;
      }
      break;
   case EMC_TRAJ_CIRCULAR_MOVE_TYPE:
      {
         emc_traj_circular_move_msg_t *p = (emc_traj_circular_move_msg_t *)cmd;
         struct rtstepper_io_req *io;

         tpSetId(&ps->tp_queue, id);
         tpSetVmax(&ps->tp_queue, p->vel);
         tpSetAmax(&ps->tp_queue, p->acc);
         tpAddCircle(&ps->tp_queue, p->end, p->center, p->normal, p->turn);

         /* Allocate an io request transfer. */ 
         io = rtstepper_io_req_alloc(ps, id, RTSTEPPER_IO_TYPE_SPINDLE_ASYNC);       

         /* Run trajectory planner. */
         _run_tp(ps, io);

         DBG("C line=%d x_pos=%0.5f, x_master=%d y_pos=%0.5f, y_master=%d z_pos=%0.5f, z_master=%d\n", id, 
         p->end.tran.x, ps->axis[EMC_AXIS_X].master_index, 
         p->end.tran.y, ps->axis[EMC_AXIS_Y].master_index, 
         p->end.tran.z, ps->axis[EMC_AXIS_Z].master_index);

         /* Dispatch step buffer package to IO system. */
         if (rtstepper_xfr_start(ps, io, tpGetPos(&ps->tp_queue)) != EMC_R_OK)
            goto bugout;
 
         stat = EMC_R_OK;
      }
      break;
   case EMC_TASK_PLAN_PAUSE_TYPE:
      {
         /* Wait for any current IO to finish. */
         rtstepper_xfr_wait(ps);
               
         if ((ps->state_bits & EMC_STATE_CANCEL_BIT) == 0)
         {
            /* M0, M1 and M60 causes a pause. A user resume command will clear the pause. */
            stat = EMC_R_PROGRAM_PAUSED;
         }
         else
            stat = EMC_R_OK;
      }
      break;
   case EMC_TRAJ_SET_TERM_COND_TYPE:
      {
         emc_traj_set_term_cond_msg_t *p = (emc_traj_set_term_cond_msg_t *)cmd;
         
         DBG("Set blending %s\n", (p->cond == TC_TERM_COND_BLEND) ? "on" : "off");

         /* Set by G64 or G61. Note G64 'tolerance' parameter is not supported. */
         tpSetTermCond(&ps->tp_queue, p->cond);

         stat = EMC_R_OK;
      }
      break;
   case EMC_TRAJ_DELAY_TYPE:
      {
         emc_traj_delay_msg_t *p = (emc_traj_delay_msg_t *)cmd;
         double step, delay=p->delay;
         
         if (delay > 0.0)
         {
            /* Wait for any current IO to finish. */
            rtstepper_xfr_wait(ps);

            /* Now perform the delay. */
            step = 1.0;
            while (step < delay)
            {
               if (ps->state_bits & EMC_STATE_ESTOP_BIT || ps->state_bits & EMC_STATE_CANCEL_BIT)
               {
                  delay = 0.0;
                  break;
               }
               esleep(step);
               delay -= step;
            }
            if (delay > 0.0)
               esleep(delay);
         }
         stat = EMC_R_OK;
      }
      break;
   case EMC_SYSTEM_CMD_TYPE:
      {
         emc_system_cmd_msg_t *p = (emc_system_cmd_msg_t *)cmd;

         /* Wait for any current IO to finish. */
         rtstepper_xfr_wait(ps);

         if ((ps->state_bits & EMC_STATE_CANCEL_BIT) == 0)
         {
            /* Call mcode python plugin (m3, m4, m5, m7, m8, m9 & user_defined). */
            stat = emc_plugin_cb(p->index, p->p_number, p->q_number);
         }
         else
            stat = EMC_R_OK;
      }
      break;
   case EMC_TASK_PLAN_END_TYPE:
      FINISH();    /* M2 or M30 */

      /* Wait for current IO to finish. */
      rtstepper_xfr_wait(ps);

      stat = EMC_R_OK;
      break;
   case EMC_START_SPEED_FEED_SYNCH:
      /* Check dongle FW for index pulse support (g76, g33). */
      if (ps->input0_abort_enabled == 0 && ps->fd_table.board_rev > RTSTEPPER_BRD_d && ps->icount_period > 0) 
      {
         emc_start_speed_feed_synch_msg_t *p = (emc_start_speed_feed_synch_msg_t *)cmd;

         /* Wait for current PC IO to finish. */
         rtstepper_xfr_wait(ps);

         if ((ps->state_bits & EMC_STATE_CANCEL_BIT) == 0)
         {
            /* Wait for IO in the dongle to finish. */
            rtstepper_dongle_empty_wait(ps);

            /* Send dongle sync_start command. */
            rtstepper_ctrl_start(ps, STEP_SYNC_START_SET, 0);

            /* Wait for above sync_start command to complete. */
            rtstepper_dongle_sync_start_wait(ps);

            /* Convert feed_per_revolution (distance per revolution) to feed_per_second. */
            ps->sync_feed_per_sec = (double)ps->step_clock / ps->icount_period_avg * p->feed_per_revolution;  /* ips = hz / tpi (inch) */
            
            /* Enable syncronized move command(s). */
            ps->sync_enabled = 1;
         }
      }
      else
      {
         if (sync_msg_cnt++ < 5)
            MSG("Warning no synchronization performed.");
      }
      stat = EMC_R_OK;
      break;
   case EMC_STOP_SPEED_FEED_SYNCH:
      /* Disable syncronized move command(s). */
      ps->sync_enabled = 0;
      stat = EMC_R_OK;
      break;
   default:
      BUG("unknown command type=%d cmd=%s\n", cmd->msg.type, lookup_message(cmd->msg.type));
      stat = EMC_R_OK; // don't consider this an error
      break;
   }

bugout:
   return stat;
}   /* _dsp_interp_cmd() */

static enum EMC_RESULT _dsp_interp_verify(struct emc_session *ps, emc_command_msg_t *cmd, int id)
{
   enum EMC_RESULT stat = EMC_R_ERROR;

   DBG("_dsp_interp_verify() cmd=%s\n", lookup_message(cmd->msg.type));

   switch (cmd->msg.type)
   {
   case EMC_TRAJ_LINEAR_MOVE_TYPE:
      {
         emc_traj_linear_move_msg_t *p = (emc_traj_linear_move_msg_t *)cmd;
         DBG("L line=%d x_pos=%0.5f, y_pos=%0.5f, z_pos=%0.5f\n", id, p->end.tran.x, p->end.tran.y, p->end.tran.z);
         ps->position = p->end;
         emc_position_post_cb(id, p->end);
         stat = EMC_R_OK;
      }
      break;
   case EMC_TRAJ_CIRCULAR_MOVE_TYPE:
      {
         emc_traj_circular_move_msg_t *p = (emc_traj_circular_move_msg_t *)cmd;
         DBG("C line=%d x_pos=%0.5f, y_pos=%0.5f, z_pos=%0.5f\n", id, p->end.tran.x, p->end.tran.y, p->end.tran.z);
         ps->position = p->end;
         emc_position_post_cb(id, p->end);
         stat = EMC_R_OK;
      }
      break;
   case EMC_TASK_PLAN_PAUSE_TYPE:
      {
         emc_position_post_cb(id, ps->position); 
         stat = EMC_R_OK;
      }
      break;
   case EMC_TRAJ_SET_TERM_COND_TYPE:
      {
         emc_position_post_cb(id, ps->position); 
         stat = EMC_R_OK;
      }
      break;
   case EMC_TRAJ_DELAY_TYPE:
      {
         emc_position_post_cb(id, ps->position); 
         stat = EMC_R_OK;
      }
      break;
   case EMC_SYSTEM_CMD_TYPE:
      {
         emc_position_post_cb(id, ps->position); 
         stat = EMC_R_OK;
      }
      break;
   case EMC_TASK_PLAN_END_TYPE:
      FINISH();    /* M2 or M30 */
      /* Post final line number for gui. */
      emc_position_post_cb(id, ps->position); 
      stat = EMC_R_OK;
      break;
   case EMC_START_SPEED_FEED_SYNCH:
      stat = EMC_R_OK;
      break;
   case EMC_STOP_SPEED_FEED_SYNCH:
      stat = EMC_R_OK;
      break;
   default:
      BUG("unknown command type=%d cmd=%s\n", cmd->msg.type, lookup_message(cmd->msg.type));
      stat = EMC_R_OK; // don't consider this an error
      break;
   }

   return stat;
}   /* _dsp_interp_verify() */

enum EMC_RESULT dsp_mdi(struct emc_session *ps, const char *mdi)
{
   enum EMC_RESULT stat;
   int retval, len;
   int line_number=0;

   DBG("dsp_mdi() cmd=%s\n", mdi);

   retval = interp.execute(mdi, line_number);
   if (retval > INTERP_MIN_ERROR)
   {
      _interp_error(retval, line_number, ps->position);
      stat = EMC_R_INTERPRETER_ERROR;
      goto bugout;
   }
   else
   {
      FINISH();
      len = interp_list.len();
      while (len)
      {
         if (_dsp_interp_cmd(ps, interp_list.get(), line_number) < EMC_R_OK)
         {            
            stat = EMC_R_ERROR;
            goto bugout;
         }
         len--;
      }
   }

   stat = EMC_R_OK;
bugout:
   return stat;
}       /* dsp_mdi() */

enum EMC_RESULT dsp_auto(struct emc_session *ps, const char *gcodefile)
{
   emc_command_msg_t *cmd;
   enum EMC_RESULT stat;
   int retval, len, id;
   char line[LINELEN];

   DBG("dsp_auto() file=%s, paused=%d\n", gcodefile, ps->state_bits & EMC_STATE_PAUSED_BIT); 

   if (ps->state_bits & EMC_STATE_PAUSED_BIT)
   {
      /* Pause is set, clear it. */
      ps->state_bits &= ~EMC_STATE_PAUSED_BIT;
   }
   else
   {
      /* Pause is NOT set, start gcode from beginning. */ 
      if((ps->gfile = fopen(gcodefile, "r")) == NULL) 
      {
         BUG("unable to open %s\n", gcodefile);
         stat = EMC_R_INVALID_GCODE_FILE;
         goto bugout;
      } 
      ps->line_number=1;  /* set file sequence number */

      /* Clear runtime stats. */
      rtstepper_clear_stats(ps);
   }

   /* Read, interpret and execute each line in the gcode file */
   while ((fgets(line, sizeof(line), ps->gfile) != NULL))
   {
      retval = interp.execute(line, ps->line_number);
      if (retval > INTERP_MIN_ERROR)
      {
         /* Interpreter error, wait for current IO to finish so the error msg is at the appropiate line #. */
         rtstepper_xfr_wait(ps);

         _interp_error(retval, ps->line_number, ps->position);
         stat = EMC_R_INTERPRETER_ERROR;
         goto bugout;
      }
      else
      {
         len = interp_list.len();
         while (len)
         {
            rtstepper_xfr_hysteresis(ps);

            if (ps->state_bits & EMC_STATE_ESTOP_BIT)
            {
               stat = EMC_R_OK;
               goto bugout;
            }

            if (ps->state_bits & EMC_STATE_CANCEL_BIT)
            {
               FINISH();
               interp_list.clear();
               stat = EMC_R_OK;
               goto bugout;     /* user cancel */          
            }

            /* Get comand and line number. */
            cmd = interp_list.get();
            id = interp_list.get_line_number(); /* Note, Mn commands don't increment the line number. */

            if ((stat = _dsp_interp_cmd(ps, cmd, id)) != EMC_R_OK)
            {
               if (stat == EMC_R_PROGRAM_PAUSED)
               {
                  /* Program is paused (M0, M1 or M60). */
                  emc_paused_post_cb(ps);

                  /* Update the display with the mcode line number. */
                  emc_position_post_cb(ps->line_number, ps->position); 

                  ps->line_number++;
                  return stat;
               }
               else
               {
                  stat = EMC_R_ERROR;
                  goto bugout;
               }
            }
            len--;
         }
      }
      ps->line_number++;
   }

   stat = EMC_R_OK;

bugout:
   if (ps->gfile != NULL)
      fclose(ps->gfile);
   return stat;
}       /* dsp_auto() */

enum EMC_RESULT dsp_verify(struct emc_session *ps, const char *gcodefile)
{
   emc_command_msg_t *cmd;
   enum EMC_RESULT stat;
   int retval, len, id;
   char line[LINELEN];

   DBG("dsp_verify() file=%s\n", gcodefile); 

   /* Force "All Zero" after verify. */
   ps->state_bits &= ~EMC_STATE_HOMED_BIT;

   if((ps->gfile = fopen(gcodefile, "r")) == NULL) 
   {
      BUG("unable to open %s\n", gcodefile);
      stat = EMC_R_INVALID_GCODE_FILE;
      goto bugout;
   } 
   ps->line_number=1;

   /* Read, interpret and execute each line in the gcode file */
   while ((fgets(line, sizeof(line), ps->gfile) != NULL))
   {
      if (ps->state_bits & EMC_STATE_CANCEL_BIT)
      {
         FINISH();      /* user cancel */
         interp_list.clear();
         emc_position_post_cb(ps->line_number, ps->position); 
         goto bugout;               
      }

      retval = interp.execute(line, ps->line_number);
      if (retval > INTERP_MIN_ERROR)
      {
         _interp_error(retval, ps->line_number, ps->position);
         stat = EMC_R_INTERPRETER_ERROR;
         goto bugout;
      }
      else
      {
         len = interp_list.len();
         while (len)
         {
            if (ps->state_bits & EMC_STATE_CANCEL_BIT)
            {
               FINISH();      /* user cancel */
               interp_list.clear();
               emc_position_post_cb(ps->line_number, ps->position); 
               goto bugout;               
            }

            cmd = interp_list.get();
            id = interp_list.get_line_number(); 
            if ((stat = _dsp_interp_verify(ps, cmd, id)) != EMC_R_OK)
            {
               stat = EMC_R_ERROR;
               goto bugout;
            }

            /* Do small delay so we don't overrun the gui with position updates. */
            //esleep(0.01);
            esleep(0.2);

            len--;
         }
      }
      ps->line_number++;
   }

   stat = EMC_R_OK;

bugout:
   if (ps->gfile != NULL)
      fclose(ps->gfile);
   return stat;
}       /* dsp_verify() */

enum EMC_RESULT dsp_auto_cancel_set(struct emc_session *ps)
{
   ps->state_bits |= EMC_STATE_CANCEL_BIT;
   MSG("User cancel...\n");
   return EMC_R_OK;
}

enum EMC_RESULT dsp_auto_cancel_clear(struct emc_session *ps)
{
   ps->state_bits &= ~EMC_STATE_CANCEL_BIT;
   return EMC_R_OK;
}

enum EMC_RESULT dsp_verify_cancel_set(struct emc_session *ps)
{
   return dsp_auto_cancel_set(ps);
}

enum EMC_RESULT dsp_verify_cancel_clear(struct emc_session *ps)
{
   return dsp_auto_cancel_clear(ps);
}

enum EMC_RESULT dsp_paused_clear(struct emc_session *ps)
{
   ps->state_bits &= ~EMC_STATE_PAUSED_BIT;
   return EMC_R_OK;
}

enum EMC_RESULT dsp_estop(struct emc_session *ps)
{
   if (ps->state_bits & EMC_STATE_ESTOP_BIT)
      return EMC_R_OK;
   rtstepper_estop(ps, RTSTEPPER_MECH_THREAD);
   emc_estop_post_cb(ps);
   MSG("User estop...\n");
   return EMC_R_OK;
}  /* dsp_estop() */

enum EMC_RESULT dsp_estop_reset(struct emc_session *ps)
{
   enum EMC_RESULT stat;

   MSG("User estop_reset...\n");
   ps->state_bits &= ~(EMC_STATE_ESTOP_BIT | EMC_STATE_PAUSED_BIT | EMC_STATE_CANCEL_BIT);
   FINISH();
   interp_list.clear();
   /* Don't reset backlash compensation, xfr_cancel() will force dsp_home() if needed. DES 10/15/2017 */
   //reset_screw_comp(ps);
   rtstepper_close(ps);
   if ((stat = rtstepper_open(ps)) != EMC_R_OK)
      emc_estop_post_cb(ps);
   sync_msg_cnt = 0;
   return stat;
}  /* dsp_estop_reset() */

enum EMC_RESULT dsp_home(struct emc_session *ps)
{
   EmcPose pos;

   /* Set origin. */
   pos.tran.x = 0.0;
   pos.tran.y = 0.0;
   pos.tran.z = 0.0;
   pos.a = 0.0;
   pos.b = 0.0;
   pos.c = 0.0;
   pos.u = 0.0;
   pos.v = 0.0;
   pos.w = 0.0;
   ps->state_bits |= EMC_STATE_HOMED_BIT;
   dsp_position_set(ps, pos);
   return EMC_R_OK;
}  /* dsp_home() */

enum EMC_RESULT dsp_position_set(struct emc_session *ps, EmcPose pos)
{
   ps->position = pos;
   interp.init();
   tpSetPos(&ps->tp_queue, ps->position); 
   rtstepper_position_set(ps, ps->position);
   reset_screw_comp(ps);   /* reset leadscrew backlash compensation */
   emc_position_post_cb(0, ps->position);
   return EMC_R_OK;
}  /* dsp_position_set() */

enum EMC_RESULT dsp_din_abort_enable(struct emc_session *ps, int input_num)
{
   enum EMC_RESULT stat=EMC_R_ERROR;

   if (input_num == INPUT0_NUM)
      ps->input0_abort_enabled = 1;
   else if (input_num == INPUT1_NUM)
      ps->input1_abort_enabled = 1;
   else if (input_num == INPUT2_NUM)
      ps->input2_abort_enabled = 1;
   else if (input_num == INPUT3_NUM)
      ps->input3_abort_enabled = 1;
   else
   {
      BUG("invalid input number=%d, expected 0-3\n", input_num);
      goto bugout;
   }

   stat = EMC_R_OK;
bugout:
   return stat;   
}

enum EMC_RESULT dsp_din_abort_disable(struct emc_session *ps, int input_num)
{
   enum EMC_RESULT stat=EMC_R_ERROR;

   if (input_num == INPUT0_NUM)
      ps->input0_abort_enabled = 0;
   else if (input_num == INPUT1_NUM)
      ps->input1_abort_enabled = 0;
   else if (input_num == INPUT2_NUM)
      ps->input2_abort_enabled = 0;
   else if (input_num == INPUT3_NUM)
      ps->input3_abort_enabled = 0;
   else
   {
      BUG("invalid input number=%d, expected 0-3\n", input_num);
      goto bugout;
   }

   stat = EMC_R_OK;
bugout:
   return stat;   
}

enum EMC_RESULT dsp_dout_set(struct emc_session *ps, int output_num)
{
   enum EMC_RESULT stat=EMC_R_ERROR;
   enum STEP_CMD cmd;

   if (output_num == OUTPUT0_NUM)
      cmd = STEP_OUTPUT0_SET;
   else if (output_num == OUTPUT1_NUM)
      cmd = STEP_OUTPUT1_SET;
   else if (output_num == OUTPUT2_NUM)
      cmd = STEP_OUTPUT2_SET;
   else
   {
      BUG("invalid output number=%d, expected 0-2\n", output_num);
      goto bugout;
   }

   /* Add usb control req to dongle thread queue. */
   rtstepper_ctrl_start(ps, cmd, 0);

   stat = EMC_R_OK;
bugout:
   return stat;   
}

enum EMC_RESULT dsp_dout_clear(struct emc_session *ps, int output_num)
{
   enum EMC_RESULT stat=EMC_R_ERROR;
   enum STEP_CMD cmd;

   if (output_num == OUTPUT0_NUM)
      cmd = STEP_OUTPUT0_CLEAR;
   else if (output_num == OUTPUT1_NUM)
      cmd = STEP_OUTPUT1_CLEAR;
   else if (output_num == OUTPUT2_NUM)
      cmd = STEP_OUTPUT2_CLEAR;
   else
   {
      BUG("invalid output number=%d, expected 0-2\n", output_num);
      goto bugout;
   }

   /* Add usb control req to dongle thread queue. */
   rtstepper_ctrl_start(ps, cmd, 0);

   stat = EMC_R_OK;
bugout:
   return stat;
}

enum EMC_RESULT dsp_output_mode(struct emc_session *ps, int output_num, int param)
{
   enum EMC_RESULT stat=EMC_R_ERROR;
   enum STEP_CMD cmd;

   /* Note, OUTPUT2 is digital only, so not changeable. */

   if (output_num == OUTPUT0_NUM && param < OMP_MAX)
      cmd = STEP_OUTPUT0_MODE;
   else if (output_num == OUTPUT1_NUM && param < OMP_MAX)
      cmd = STEP_OUTPUT1_MODE;
   else
   {
      BUG("invalid output_mode command: output_num=%d, param=%d\n", output_num, param);
      goto bugout;
   }

   /* Add usb control req to dongle thread queue. */
   rtstepper_ctrl_start(ps, cmd, param);

   stat = EMC_R_OK;
bugout:
   return stat;   
}

enum EMC_RESULT dsp_output_pwm(struct emc_session *ps, int output_num, int param)
{
   enum EMC_RESULT stat=EMC_R_ERROR;
   enum STEP_CMD cmd;

   if (output_num == OUTPUT0_NUM && param < 256)
      cmd = STEP_OUTPUT0_PWM;
   else if (output_num == OUTPUT1_NUM && param < 256)
      cmd = STEP_OUTPUT1_PWM;
   else
   {
      BUG("invalid output_pwm command: output_num=%d, param=%d\n", output_num, param);
      goto bugout;
   }

   /* Add usb control req to dongle thread queue. */
   rtstepper_ctrl_start(ps, cmd, param);

   stat = EMC_R_OK;
bugout:
   return stat;
}

enum EMC_RESULT dsp_input_mode(struct emc_session *ps, int input_num, int param)
{
   enum EMC_RESULT stat=EMC_R_ERROR;
   enum STEP_CMD cmd;

   /* Note, INPUT0 is digital only, so not changeable. */

   if (input_num == INPUT1_NUM && param < IMP_MAX)
      cmd = STEP_INPUT1_MODE;
   else if (input_num == INPUT2_NUM && param < IMP_MAX)
      cmd = STEP_INPUT2_MODE;
   else if (input_num == INPUT3_NUM && param < IMP_MAX)
      cmd = STEP_INPUT3_MODE;
   else
   {
      BUG("invalid input_mode command: input_num=%d, param=%d\n", input_num, param);
      goto bugout;
   }

   /* Add usb control req to dongle thread queue. */
   rtstepper_ctrl_start(ps, cmd, param);

   stat = EMC_R_OK;
bugout:
   return stat;
}


enum EMC_RESULT dsp_io_done_wait(struct emc_session *ps)
{
   return rtstepper_xfr_wait(ps);
}

enum EMC_RESULT dsp_open(struct emc_session *ps)
{
   enum EMC_RESULT stat = EMC_R_ERROR;
   int r;

   DBG("dsp_open()\n");

   /* Initialize gcode interpreter. */
   interp.ini_load(ps->ini_file);
   r = interp.init();   /* clears interp.file() */
   if (r > INTERP_MIN_ERROR)
   {
      BUG("dsp_open() interpreter error: %d\n", r);
      goto bugout;
   }

   /* Initialize trajectory planner. */
   if (tpCreate(&ps->tp_queue, DEFAULT_TC_QUEUE_SIZE, ps->tc_queue) == -1)
   {
      BUG("dsp_open() unabled to initialize trajectory planner\n");
      goto bugout;
   }
   tpSetCycleTime(&ps->tp_queue, ps->cycle_time);
   tpSetPos(&ps->tp_queue, ps->position);
   tpSetVlimit(&ps->tp_queue, ps->maxVelocity);

   stat = EMC_R_OK;
bugout:
   return stat;
}  /* dsp_open() */

enum EMC_RESULT dsp_close(struct emc_session *ps)
{
   DBG("dsp_close()\n");
   interp.exit();
   tpDelete(&ps->tp_queue);
   return EMC_R_OK;
}  /* dsp_close() */
