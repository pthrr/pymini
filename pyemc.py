#!/usr/bin/python
# pyemc.py - A cytypes library interface for rtstepperemc.
#
# (c) 2014-2015 Copyright Eckler Software
#
# Author: David Suffield, dsuffiel@ecklersoft.com
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of version 2 of the GNU General Public License as published by
# the Free Software Foundation.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
#
# Upstream patches are welcome. Any patches submitted to the author must be
# unencumbered (ie: no Copyright or License).
#
# History:
#
# 12/16/2014 - New

import os, sys, logging
from ctypes import cdll, c_int, c_char_p, c_void_p, c_double, c_long, c_void_p, byref, cast, Structure, POINTER, CFUNCTYPE
import sys, importlib
from version import Version

class MechResult(object):
    EMC_R_INVALID_GCODE_FILE = -11
    EMC_R_INTERPRETER_ERROR = -10
    RTSTEPPER_R_IO_TIMEDOUT = -9
    RTSTEPPER_R_DEVICE_UNAVAILABLE = -8
    RTSTEPPER_R_REQ_ERROR = -7
    RTSTEPPER_R_MALLOC_ERROR = -6
    RTSTEPPER_R_IO_ERROR = -5
    EMC_R_INVALID_INI_KEY = -4
    EMC_R_INVALID_INI_FILE = -3
    EMC_R_TIMEOUT = -2
    EMC_R_ERROR = -1
    EMC_R_OK = 0
    RTSTEPPER_R_INPUT_TRUE = 1
    RTSTEPPER_R_INPUT_FALSE = 2
    EMC_R_PROGRAM_PAUSED = 3

class MechStateBit(object):
    ABORT = 0x1
    EMPTY = 0x2
    INPUT0 = 0x8
    INPUT1 = 0x10
    INPUT2 = 0x20
    INPUT3 = 0x40
    ESTOP = 0x10000
    PAUSED = 0x20000
    HOMED = 0x40000
    CANCEL = 0x80000

class MechOutputNum(object):
    OUTPUT0 = 0
    OUTPUT1 = 1
    OUTPUT2 = 2

class EmcPose(Structure):
    _fields_ = [("x", c_double),
       ("y", c_double),
       ("z", c_double),
       ("a", c_double),
       ("b", c_double),
       ("c", c_double),
       ("u", c_double),
       ("v", c_double),
       ("w", c_double)]

class mech_pos(Structure):
    _fields_ = [("id", c_int), ("pos", EmcPose)]

# Define dll to python callback functions. First param is the return type.
LOGGER_CB_FUNC = CFUNCTYPE(None, c_char_p)
POSITION_CB_FUNC = CFUNCTYPE(None, c_int, POINTER(mech_pos))
PLUGIN_CB_FUNC = CFUNCTYPE(c_int, c_int, c_double, c_double)
GUI_EVENT_CB_FUNC = CFUNCTYPE(None, c_int)

class EmcMech(object):

    def __init__(self):
        try:
            if (os.path.dirname(Version.dll) == ""):
                self.LIBRARY_FILE = os.path.join(os.path.dirname(os.path.realpath(sys.argv[0])), Version.dll)
            else:
                self.LIBRARY_FILE = os.path.realpath(Version.dll)
            logging.info("Loading library: %s" % (self.LIBRARY_FILE))
            self.lib = cdll.LoadLibrary(self.LIBRARY_FILE)

            # void *emc_ui_open(const char *home, const char *ini_file)
            self._open = self.lib.emc_ui_open
            self._open.argtypes = [c_char_p, c_char_p]
            self._open.restype = c_void_p

            # enum EMC_RESULT emc_ui_close(void *hd)
            self._close = self.lib.emc_ui_close
            self._close.argtypes = [c_void_p]
            self._close.restype = c_int

            # enum EMC_RESULT emc_ui_version_get(const char **ver)
            self._get_version = self.lib.emc_ui_version_get
            self._get_version.argtypes = [POINTER(c_void_p)]
            self._get_version.restype = c_int

            # enum EMC_RESULT emc_ui_logger_register_cb(void (*fp)(const char *msg))
            self.cb_logger = LOGGER_CB_FUNC(self.call_logger_cb)
            self._register_logger_cb = self.lib.emc_ui_logger_register_cb
            self._register_logger_cb.argtype = [self.cb_logger]
            self._register_logger_cb.restype = c_int

            # enum EMC_RESULT emc_ui_plugin_register_cb(void (*fp)(const char *msg))
            self.cb_plugin = PLUGIN_CB_FUNC(self.call_plugin_cb)
            self._register_plugin_cb = self.lib.emc_ui_plugin_register_cb
            self._register_plugin_cb.argtype = [self.cb_plugin]
            self._register_plugin_cb.restype = c_int

            # enum EMC_RESULT emc_ui_gui_event_register_cb(void (*fp)(int cmd))
            self.cb_gui_event = GUI_EVENT_CB_FUNC(self.post_gui_event_cb)
            self._register_gui_event_cb = self.lib.emc_ui_gui_event_register_cb
            self._register_gui_event_cb.argtype = [self.cb_gui_event]
            self._register_gui_event_cb.restype = c_int

            # enum EMC_RESULT emc_ui_position_register_cb(void (*fp)(int cmd, struct post_position_py *pos))
            self.cb_position = POSITION_CB_FUNC(self.post_position_cb)
            self._register_position_cb = self.lib.emc_ui_position_register_cb
            self._register_position_cb.argtype = [self.cb_position]
            self._register_position_cb.restype = c_int

            # enum EMC_RESULT emc_ui_position_get(void *hd, struct emcpose_py *pos)
            self._get_position = self.lib.emc_ui_position_get
            self._get_position.argtypes = [c_void_p, POINTER(EmcPose)]
            self._get_position.restype = c_int

            # DLL_EXPORT enum EMC_RESULT emc_ui_position_set(void *hd, struct emcpose_py *pospy)
            self._set_position = self.lib.emc_ui_position_set
            self._set_position.argtypes = [c_void_p, POINTER(EmcPose)]
            self._set_position.restype = c_int

            # enum EMC_RESULT emc_ui_mdi_cmd(void *hd, const char *mdi)
            self._mdi_cmd = self.lib.emc_ui_mdi_cmd
            self._mdi_cmd.argtypes = [c_void_p, c_char_p]
            self._mdi_cmd.restype = c_int

            # enum EMC_RESULT emc_ui_auto_cmd(void *hd, const char *gcodefile)
            self._auto_cmd = self.lib.emc_ui_auto_cmd
            self._auto_cmd.argtypes = [c_void_p, c_char_p]
            self._auto_cmd.restype = c_int

            # enum EMC_RESULT emc_ui_auto_cancel_set(void *hd)
            self._auto_cancel_set = self.lib.emc_ui_auto_cancel_set
            self._auto_cancel_set.argtypes = [c_void_p]
            self._auto_cancel_set.restype= c_int

            # enum EMC_RESULT emc_ui_auto_cancel_clear(void *hd)
            self._auto_cancel_clear = self.lib.emc_ui_auto_cancel_clear
            self._auto_cancel_clear.argtypes = [c_void_p]
            self._auto_cancel_clear.restype= c_int

            # enum EMC_RESULT emc_ui_paused_clear(void *hd)
            self._paused_clear = self.lib.emc_ui_paused_clear
            self._paused_clear.argtypes = [c_void_p]
            self._paused_clear.restype= c_int

            # enum EMC_RESULT emc_ui_io_done_wait(void *hd)
            self._wait_io_done = self.lib.emc_ui_io_done_wait
            self._wait_io_done.argtypes = [c_void_p]
            self._wait_io_done.restype = c_int

            # enum EMC_RESULT emc_ui_home(void *hd)
            self._home = self.lib.emc_ui_home
            self._home.argtypes = [c_void_p]
            self._home.restype= c_int

            # enum EMC_RESULT emc_ui_state_get(void *hd, unsigned long *stat)
            self._get_state = self.lib.emc_ui_state_get
            self._get_state.argtypes = [c_void_p, POINTER(c_long)]
            self._get_state.restype= c_int

            # enum EMC_RESULT emc_ui_estop(void *hd)
            self._estop = self.lib.emc_ui_estop
            self._estop.argtypes = [c_void_p]
            self._estop.restype= c_int

            # enum EMC_RESULT emc_ui_estop_reset(void *hd)
            self._estop_reset = self.lib.emc_ui_estop_reset
            self._estop_reset.argtypes = [c_void_p]
            self._estop_reset.restype= c_int
            
            # enum EMC_RESULT emc_ui_din_abort_disable(void *hd, int input_num)
            self._din_abort_disable = self.lib.emc_ui_din_abort_disable
            self._din_abort_disable.argtypes = [c_void_p, c_int]
            self._din_abort_disable.restype= c_int

            # enum EMC_RESULT emc_ui_din_abort_enable(void *hd, int input_num)
            self._din_abort_enable = self.lib.emc_ui_din_abort_enable
            self._din_abort_enable.argtypes = [c_void_p, c_int]
            self._din_abort_enable.restype= c_int
            
            # enum EMC_RESULT emc_ui_din_frequency_get(void *hd, int input_num, double *value)
            self._din_frequency_get = self.lib.emc_ui_din_frequency_get
            self._din_frequency_get.argtypes = [c_void_p, c_int, POINTER(c_double)]
            self._din_frequency_get.restype= c_int

            # enum EMC_RESULT emc_ui_din_frequency_avg_get(void *hd, int input_num, double *value)
            self._din_frequency_avg_get = self.lib.emc_ui_din_frequency_avg_get
            self._din_frequency_avg_get.argtypes = [c_void_p, c_int, POINTER(c_double)]
            self._din_frequency_avg_get.restype= c_int

            # enum EMC_RESULT emc_ui_din_frequency_max_get(void *hd, int input_num, double *value)
            self._din_frequency_max_get = self.lib.emc_ui_din_frequency_max_get
            self._din_frequency_max_get.argtypes = [c_void_p, c_int, POINTER(c_double)]
            self._din_frequency_max_get.restype= c_int

            # enum EMC_RESULT emc_ui_din_frequency_min_get(void *hd, int input_num, double *value)
            self._din_frequency_min_get = self.lib.emc_ui_din_frequency_min_get
            self._din_frequency_min_get.argtypes = [c_void_p, c_int, POINTER(c_double)]
            self._din_frequency_min_get.restype= c_int

            # enum EMC_RESULT emc_ui_verify_cmd(void *hd, const char *gcodefile)
            self._verify_cmd = self.lib.emc_ui_verify_cmd
            self._verify_cmd.argtypes = [c_void_p, c_char_p]
            self._verify_cmd.restype = c_int

            # enum EMC_RESULT emc_ui_verify_cancel_set(void *hd)
            self._verify_cancel_set = self.lib.emc_ui_verify_cancel_set
            self._verify_cancel_set.argtypes = [c_void_p]
            self._verify_cancel_set.restype= c_int

            # enum EMC_RESULT emc_ui_verify_cancel_clear(void *hd)
            self._verify_cancel_clear = self.lib.emc_ui_verify_cancel_clear
            self._verify_cancel_clear.argtypes = [c_void_p]
            self._verify_cancel_clear.restype= c_int

            # enum EMC_RESULT emc_ui_test(const char *snum)
            self._test = self.lib.emc_ui_test
            self._test.argtypes = [c_char_p]
            self._test.restype = c_int

            # enum EMC_RESULT emc_ui_dout_set(void *hd, int output_num)
            self._dout_set = self.lib.emc_ui_dout_set
            self._dout_set.argtypes = [c_void_p, c_int]
            self._dout_set.restype= c_int

            # enum EMC_RESULT emc_ui_dout_clear(void *hd, int output_num)
            self._dout_clear = self.lib.emc_ui_dout_clear
            self._dout_clear.argtypes = [c_void_p, c_int]
            self._dout_clear.restype= c_int

            # enum EMC_RESULT emc_ui_output_mode(void *hd, int output_num, int param)
            self._output_mode = self.lib.emc_ui_output_mode
            self._output_mode.argtypes = [c_void_p, c_int, c_int]
            self._output_mode.restype= c_int

            # enum EMC_RESULT emc_ui_output_pwm(void *hd, int output_num, int param)
            self._output_pwm = self.lib.emc_ui_output_pwm
            self._output_pwm.argtypes = [c_void_p, c_int, c_int]
            self._output_pwm.restype= c_int

            # enum EMC_RESULT emc_ui_input_mode(void *hd, int input_num, int param)
            self._input_mode = self.lib.emc_ui_input_mode
            self._input_mode.argtypes = [c_void_p, c_int, c_int]
            self._input_mode.restype= c_int

            # enum EMC_RESULT emc_ui_input_adc_get(void *hd, int input_num, int *value)
            self._input_adc_get = self.lib.emc_ui_input_adc_get
            self._input_adc_get.argtypes = [c_void_p, c_int, POINTER(c_int)]
            self._input_adc_get.restype= c_int

        except Exception as err:
            logging.error("unable to load library: %s %s" % (self.LIBRARY_FILE, err))

    ################################################################################################################
    def call_logger_cb(self, msg):
        logging.info(msg.rstrip())

    ################################################################################################################
    def call_plugin_cb(self, mcode, p_num, q_num):
        module = "plugin.m%d" % (mcode)
        try:
            plugin = importlib.import_module(module)
            return plugin.run(self, p_num, q_num)
        except Exception as err:
            logging.error("unable to load: %s.py %s" % (module, err))
            return MechResult.EMC_R_ERROR

    ################################################################################################################
    def post_gui_event_cb(self, cmd):
        if (self.guiq != None):
            m = {}
            m['id'] = cmd
            self.guiq.put(m)

    ################################################################################################################
    def post_position_cb(self, cmd, post_p):
        post = post_p[0]
        if (self.guiq != None):
            m = {}
            m['id'] = cmd
            m['pos'] = {'x':post.pos.x, 'y':post.pos.y, 'z':post.pos.z, 'a':post.pos.a}
            m['line'] = post.id
            self.guiq.put(m)

    #############################################################################################################
    def mdi_cmd(self, cmd):
        return self._mdi_cmd(self.hd, cmd.encode('ascii'))

    #############################################################################################################
    def auto_cmd(self, gcodefile):
        return self._auto_cmd(self.hd, gcodefile.encode('ascii'))

    #############################################################################################################
    def auto_cancel_set(self):
        return self._auto_cancel_set(self.hd)

    #############################################################################################################
    def auto_cancel_clear(self):
        return self._auto_cancel_clear(self.hd)

    #############################################################################################################
    def verify_cmd(self, gcodefile):
        return self._verify_cmd(self.hd, gcodefile.encode('ascii'))

    #############################################################################################################
    def verify_cancel_set(self):
        return self._verify_cancel_set(self.hd)

    #############################################################################################################
    def verify_cancel_clear(self):
        return self._verify_cancel_clear(self.hd)

    #############################################################################################################
    def paused_clear(self):
        return self._paused_clear(self.hd)

    #############################################################################################################
    def wait_io_done(self):
        return self._wait_io_done(self.hd)

    #############################################################################################################
    def home(self):
        return self._home(self.hd)

    #############################################################################################################
    def get_position(self):
        pos = EmcPose()
        self._get_position(self.hd, byref(pos))
        cur_pos = {'x':pos.x, 'y':pos.y, 'z':pos.z, 'a':pos.a, 'b':pos.b, 'c':pos.c, 'u':pos.u, 'v':pos.v, 'w':pos.w}
        return cur_pos

    #############################################################################################################
    def set_position(self, position):
        pos = EmcPose()
        pos.x = position.get('x', 0.0)
        pos.y = position.get('y', 0.0)
        pos.z = position.get('z', 0.0)
        pos.a = position.get('a', 0.0)
        pos.b = position.get('b', 0.0)
        pos.c = position.get('c', 0.0)
        pos.u = position.get('u', 0.0)
        pos.v = position.get('v', 0.0)
        pos.w = position.get('w', 0.0)
        return self._set_position(self.hd, byref(pos))

    #############################################################################################################
    def get_state(self):
        s = c_long()
        self._get_state(self.hd, byref(s))
        return s.value

    #############################################################################################################
    def estop(self):
        return self._estop(self.hd)

    #############################################################################################################
    def estop_reset(self):
        return self._estop_reset(self.hd)

    #############################################################################################################
    def din_abort_disable(self, input_num):
        return self._din_abort_disable(self.hd, input_num)

    #############################################################################################################
    def din_abort_enable(self, input_num):
        return self._din_abort_enable(self.hd, input_num)

    #############################################################################################################
    def din_frequency_get(self, input_num):
        s = c_double()
        self._din_frequency_get(self.hd, input_num, byref(s))
        return s.value

    #############################################################################################################
    def din_frequency_avg_get(self, input_num):
        s = c_double()
        self._din_frequency_avg_get(self.hd, input_num, byref(s))
        return s.value

    #############################################################################################################
    def din_frequency_max_get(self, input_num):
        s = c_double()
        self._din_frequency_max_get(self.hd, input_num, byref(s))
        return s.value

    #############################################################################################################
    def din_frequency_min_get(self, input_num):
        s = c_double()
        self._din_frequency_min_get(self.hd, input_num, byref(s))
        return s.value

    #############################################################################################################
    def test(self, snum):
        return self._test(snum.encode('ascii'))

    #############################################################################################################
    def dout_set(self, output_num):
        return self._dout_set(self.hd, output_num)

    #############################################################################################################
    def dout_clear(self, output_num):
        return self._dout_clear(self.hd, output_num)

    #############################################################################################################
    def output_mode(self, output_num, value):
        return self._output_mode(self.hd, output_num, value)

    #############################################################################################################
    def output_pwm(self, output_num, value):
        return self._output_pwm(self.hd, output_num, value)

    #############################################################################################################
    def input_mode(self, input_num, value):
        return self._input_mode(self.hd, input_num, value)

    #############################################################################################################
    def input_adc_get(self, input_num):
        s = c_int()
        self._input_adc_get(self.hd, input_num, byref(s))
        return s.value

    #############################################################################################################
    def get_version(self):
        p = c_void_p()
        self._get_version(byref(p))
        p = cast(p, c_char_p)
        return p.value

    #############################################################################################################
    def register_logger_cb(self):
        return self._register_logger_cb(self.cb_logger)

    #############################################################################################################
    def register_event_cb(self, queue):
        self.guiq = queue
        self.register_gui_event_cb()
        self.register_position_cb()
        self.register_plugin_cb()

    ################################################################################################################
    def register_plugin_cb(self):
        return self._register_plugin_cb(self.cb_plugin)

    #############################################################################################################
    def register_gui_event_cb(self):
        return self._register_gui_event_cb(self.cb_gui_event)

    #############################################################################################################
    def register_position_cb(self):
        return self._register_position_cb(self.cb_position)

    ################################################################################################################
    def open(self, home_dir, ini_file="rtstepper.ini"):
        self.inifile = ini_file
        self.hd = self._open(home_dir.encode('ascii'), ini_file.encode('ascii'))

    #############################################################################################################
    def close(self):
        return self._close(self.hd)

################################################################################################################
if __name__ == "__main__":
    # Configure logging for the application.
    logging.basicConfig(filename='log.txt',
                        level=logging.DEBUG,
                        format='%(asctime)s:%(levelname)s:%(filename)s:%(lineno)s:%(message)s')

    mech = EmcMech()
    mech.register_logger_cb()
    mech.open("rtstepper.ini")
    print(mech.get_version())
    mech.mdi_cmd("g1 x0.1 f6")
    mech.wait_io_done()
    print(mech.get_position())
    print(mech.get_state())
    print(mech.home())
    print(mech.get_state())
    mech.close()
