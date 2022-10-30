#!/usr/bin/python
# pymini.py - A python GUI that interfaces with rtstepperemc library.
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
# 12/12/2014 - New

import logging
import logging.handlers
import threading
import sys, os, getopt
try:
    import queue
    import tkinter
    import tkinter.filedialog as filedialog
    import configparser
except ImportError:
    import Queue as queue
    import Tkinter as tkinter
    import tkFileDialog as filedialog
    import ConfigParser as configparser
import time
import pyemc
import backplot
import led
import tooltable, fixture
from pyemc import MechStateBit
from version import Version

axis_name = ["na", "X", "Y", "Z", "A"]  # AxisSel to motor_name map
pos_name = ["na", "x", "y", "z", "a"]  # AxisSel to GetPosition_Eng() map

# Note, for windows HOME = "C:\Documents and Settings\user_name"
HOME_DIR = "%s/.%s" % (os.path.expanduser("~"), Version.name)

# For windows set tk environment for embedded python.
if (os.name == "nt"):
    tk_path = os.path.dirname(os.path.realpath(sys.argv[0]))
    os.environ['TCL_LIBRARY'] = "%s/tcl8.6" % (tk_path)
    os.environ['TK_LIBRARY'] = "%s/tk8.6" % (tk_path)

class IniFile(object):
    name = Version.ini

class Panel(object):
    MAX_LINE = 6

class AxisSel(object):
    """Enumeration of buttons"""
    X = 1
    Y = 2
    Z = 3
    A = 4

class JogTypeSel(object):
    """Enumeration of buttons"""
    INC = 1
    ABS = 2

class JogSel(object):
    """Enumeration of buttons"""
    NEG = 1
    POS = 2

class AutoSel(object):
    OPEN = 1
    RUN = 2
    VERIFY = 3

class GuiEvent(object):
    """Events from mech to gui."""
    MECH_IDLE = 1
    LOG_MSG = 2
    MECH_DEFAULT = 3
    MECH_POSITION = 4
    MECH_ESTOP = 5  # auto estop from mech
    MECH_PAUSED = 6  # M0, M1 or M60 from parser
    MECH_RPM = 7

class ButtonState(object):
    # estop, home, jog, run, mdi, resume, verify, tooltbl, fixture
    #  0,     1,   2,   3,   4,   5,      6,      7,       8
    ESTOP  = [ 1, 0, 0, 0, 0, 0, 1, 1, 1 ]
    BUSY   = [ 1, 0, 0, 0, 0, 0, 0, 0, 0 ]
    BUSY2  = [ 0, 0, 0, 0, 0, 0, 0, 0, 0 ]
    AUTO  =  [ 1, 0, 0, 2, 0, 0, 0, 0, 0 ] 
    IDLE   = [ 1, 1, 1, 1, 1, 0, 0, 1, 1 ]
    PAUSED = [ 1, 0, 0, 2, 0, 1, 0, 0, 0 ]
    RESUME = [ 1, 0, 0, 2, 0, 2, 0, 0, 0 ] 
    VERIFY = [ 0, 0, 0, 0, 0, 0, 2, 0, 0 ] 
    #          ES HO JO RU MD RE VE TO FI

class MechEvent(object):
    """Events from gui to mech."""
    CMD_RUN = 1
    CMD_MDI = 2
    CMD_ALL_ZERO = 3
    CMD_ESTOP_RESET = 4
    CMD_VERIFY = 5

def usage():
    print("%s %s %s, rt-stepper dongle application software" % (Version.name, Version.release, Version.date))
    print("(c) 2013-2020 Copyright Eckler Software")
    print("David Suffield, dsuffiel@ecklersoft.com")
    print("usage: %s [-i your_file.ini] (default=rtstepper.ini)" % (Version.name))

class LogPanelHandler(logging.Handler):
    def __init__(self, guiq):
        logging.Handler.__init__(self)
        # Ignore Debug messages, display Info, Error and above.
        self.setLevel(level=logging.INFO)
        self.guiq = guiq

    def emit(self, record):
        """ Queue up a logger message for the gui. """
        r = self.format(record)
        e = {}
        e['id'] = GuiEvent.LOG_MSG
        e['msg'] = self.format(record)
        self.guiq.put(e)

# =======================================================================
class Mech(object):
    def __init__(self, cfg, guiq, mechq, dog):
        self.tid = self.mech_thread(cfg, guiq, mechq, dog, TK_INTERVAL=1.0)
        self.tid.start()

    class mech_thread(threading.Thread):
        def __init__(self, cfg, guiq, mechq, dog, TK_INTERVAL):
            threading.Thread.__init__(self)
            self.done = threading.Event()
            self.cfg = cfg
            self.guiq = guiq
            self.mechq = mechq
            self.dog = dog
            self.TK_INTERVAL = TK_INTERVAL  # mech_thread() sleep interval in seconds
            self.hz = 0
            self.old_hz = -1

        def shutdown(self):
            self.done.set()  # stop thread()

        # Main thread() function.
        def run(self):
            if (not(self.dog.get_state() & MechStateBit.ESTOP)):
                self.post_idle(ButtonState.IDLE)

            while (1):
                # Check for thread shutdown.
                if (self.done.isSet()):
                    break

                # Sleep for interval or until shutdown.
                self.done.wait(self.TK_INTERVAL)

                # Check queue for any commands.
                while (not self.mechq.empty()):
                    e = self.mechq.get()
                    if (e['id'] == MechEvent.CMD_MDI):
                        self.cmd_mdi(e['cmd'])
                    elif (e['id'] == MechEvent.CMD_RUN):
                        self.cmd_auto(e['file'])
                    elif (e['id'] == MechEvent.CMD_ALL_ZERO):
                        self.cmd_all_zero()
                    elif (e['id'] == MechEvent.CMD_ESTOP_RESET):
                        self.cmd_estop_reset()
                    elif (e['id'] == MechEvent.CMD_VERIFY):
                        self.cmd_verify(e['file'])
                    else:
                        pass
                    e = None

                # Check dongle INPUT0 frequency counter. 
                self.hz = self.dog.din_frequency_get(0)
                if (self.old_hz != self.hz):
                    self.old_hz = self.hz
                    self.post_rpm(self.hz)

        def post_idle(self, bstate):
            """ Let gui know we are idle. """
            m = {}
            m['id'] = GuiEvent.MECH_IDLE
            m['bstate'] = bstate
            self.guiq.put(m)

        def post_rpm(self, hz):
            m = {}
            m['id'] = GuiEvent.MECH_RPM
            m['hz'] = hz
            self.guiq.put(m)

        def cmd_mdi(self, buf):
            for ln in buf.splitlines():
                if (self.done.isSet()):
                    break
                try:
                    self.dog.mdi_cmd(ln)
                except:
                    logging.error("Unable to process MDI command: %s" % (ln))
            self.dog.wait_io_done()
            if (self.dog.get_state() & MechStateBit.ESTOP):
                self.post_idle(ButtonState.ESTOP)
            else:
                self.post_idle(ButtonState.IDLE)

        def cmd_auto(self, gcodefile):
            try:
                # Execute gcode file.
                self.dog.auto_cmd(gcodefile)
            except:
                logging.error("Unable to process gcode file: %s" % (gcodefile))
            self.dog.wait_io_done()
            if (self.dog.get_state() & MechStateBit.ESTOP):
                self.post_idle(ButtonState.ESTOP)
            elif (self.dog.get_state() & MechStateBit.CANCEL):
                # Synchronize stop. Since interpreter runs asynchronous from IO reset gcode interpreter to last position.
                self.dog.auto_cancel_clear()
                self.dog.set_position(self.dog.get_position())
                self.post_idle(ButtonState.IDLE)
            elif (self.dog.get_state() & MechStateBit.PAUSED):
                self.post_idle(ButtonState.PAUSED)
            else:
                self.post_idle(ButtonState.IDLE)

        def cmd_verify(self, gcodefile):
            try:
                # Execute gcode file.
                self.dog.verify_cmd(gcodefile)
            except:
                logging.error("Unable to process gcode file: %s" % (gcodefile))
            if (self.dog.get_state() & MechStateBit.CANCEL):
                self.dog.verify_cancel_clear()
            if (self.dog.get_state() & MechStateBit.ESTOP):
                self.post_idle(ButtonState.ESTOP)
            else:
                self.post_idle(ButtonState.IDLE)

        def cmd_all_zero(self):
            self.dog.home()
            if (not(self.dog.get_state() & MechStateBit.ESTOP)):
                self.post_idle(ButtonState.IDLE)

        def cmd_estop_reset(self):
            result = self.dog.estop_reset()
            if (not(self.dog.get_state() & MechStateBit.ESTOP)):
                self.post_idle(ButtonState.IDLE)

    def close(self):
        logging.info("Mech thread closing...")
        self.tid.shutdown()
        self.tid.join()
        self.tid = None

#=======================================================================
class Gui(tkinter.Tk):
    def __init__(self, parent=None):
        tkinter.Tk.__init__(self, parent)
        self.guiq = queue.Queue()  # Mech to tkinter communication
        self.mechq = queue.Queue()  # tkinter to Mech communication
        self.sel_axis = AxisSel.X  # set axis default
        self.sel_jog_type = JogTypeSel.INC  # set jog type default
        self.green_led = tkinter.PhotoImage(data=led.GREEN)
        self.red_led = tkinter.PhotoImage(data=led.RED)
        self.orange_led = tkinter.PhotoImage(data=led.ORANGE)
        self._update()  # kickoff _update()
        self.create_widgets()
        self.bp3d = backplot.BackPlot(self.backplot_canvas)

        # Instantiate dongle.
        self.dog = pyemc.EmcMech()

        # Setup dll callbacks
        self.dog.register_logger_cb()
        self.dog.register_event_cb(self.guiq)

        # Open dll.
        self.dog.open(HOME_DIR, IniFile.name)
 
        # Kickoff mech thread().
        self.mech = Mech(self.cfg, self.guiq, self.mechq, self.dog)

    @property
    def homedir(self):
        return HOME_DIR

    @property
    def inifile(self):
        return IniFile.name
        
    #=======================================================================
    def _update(self):
        """ Check guiq for events. """
        self.proc()
        self.after(200, self._update)

    #=======================================================================
    def proc(self):
        """ Check queue for any io initiated events. """
        while (not self.guiq.empty()):
            e = self.guiq.get()
            if (e['id'] == GuiEvent.MECH_IDLE):
                self.set_idle_state(e['bstate'])
            elif (e['id'] == GuiEvent.LOG_MSG):
                self.display_logger_message(e)
            elif (e['id'] == GuiEvent.MECH_POSITION):
                self.update_position(e)
            elif (e['id'] == GuiEvent.MECH_ESTOP):
                self.set_estop_state()  # auto estop from mech
            elif (e['id'] == GuiEvent.MECH_PAUSED):
                self.set_idle_state(ButtonState.PAUSED)  # auto pause from parser
            elif (e['id'] == GuiEvent.MECH_RPM):
                self.update_rpm(e['hz'])
            else:
                logging.info("unable to process gui event %d\n" % (e['id']))
            e = None

    #=======================================================================
    def get_ini(self, section, option, default=None):
        try:
            val = self.cfg.get(section, option)
        except:
            val = default
        return val

    #=======================================================================
    def create_widgets(self):
        self.grid()
        grow=0
        self.x_val = tkinter.Label(self, relief=tkinter.GROOVE)
        self.x_val.grid(row=grow, column=0, sticky='ew')
        self.x_val.bind("<Button-1>", lambda event: self.axis_button(AxisSel.X))

        grow += 1
        self.y_val = tkinter.Label(self)
        self.y_val.grid(row=grow, column=0, sticky='ew')
        self.y_val.bind("<Button-1>", lambda event: self.axis_button(AxisSel.Y))


        grow += 1
        self.z_val = tkinter.Label(self)
        self.z_val.grid(row=grow, column=0, sticky='ew')
        self.z_val.bind("<Button-1>", lambda event: self.axis_button(AxisSel.Z))

        self.tool_table_button = tkinter.Button(self, text="Tool Offsets",
                                          command=lambda: self.edit_tool_table())
        self.tool_table_button.grid(row=grow, column=3, columnspan=2, sticky='news')
        self.fixture_button = tkinter.Button(self, text="Fixture Offsets",
                                          command=lambda: self.edit_fixture_table())
        self.fixture_button.grid(row=grow, column=5, columnspan=1, sticky='news')

        grow += 1
        self.a_val = tkinter.Label(self)
        self.a_val.grid(row=grow, column=0, sticky='ew')
        self.a_val.bind("<Button-1>", lambda event: self.axis_button(AxisSel.A))

        self.estop_button = tkinter.Button(self, text="EStop", command=self.toggle_estop)
        self.estop_button.grid(row=grow, column=3, rowspan=2, columnspan=2, sticky='news')

        self.jogneg_button = tkinter.Button(self, text="Jog X -", command=lambda: self.jog(JogSel.NEG))
        self.jogneg_button.grid(row=grow, column=1, rowspan=2, sticky='news')

        self.jogpos_button = tkinter.Button(self, text="Jog X +", command=lambda: self.jog(JogSel.POS))
        self.jogpos_button.grid(row=grow, column=2, rowspan=2, sticky='news')

        self.resume_button = tkinter.Button(self, text="Resume", command=self.resume)
        self.resume_button.grid(row=grow, column=5, rowspan=2, sticky='news')

        grow += 1
        self.home_button = tkinter.Button(self, text="All Zero", command=self.set_all_zero)
        self.home_button.grid(row=grow, column=0, sticky='news')

        grow = 0
        self.status_button = tkinter.Label(self, text="status")
        self.status_button.grid(row=grow, column=4, sticky='e')
        self.led_button = tkinter.Label(self, image=self.green_led)
        self.led_button.grid(row=grow, column=5, sticky='w')

        panelrow = 17
        lastrow = panelrow
        self.log_panel = tkinter.Text(self, state=tkinter.DISABLED, width=80, height=Panel.MAX_LINE, wrap=tkinter.NONE,
                                      bg=self.x_val['bg'], relief=tkinter.SUNKEN, borderwidth=4)
        self.log_panel.grid(row=lastrow, columnspan=6, sticky='news')

        lastrow += 1
        self.status_line_num = tkinter.Label(self)
        self.status_line_num.grid(row=lastrow, column=5, sticky='w')

        # Pipe log messages to Text widget.
        self.create_logger(self.guiq)

        # Load persistent data from .ini for MDI buttons. Note, 'realpath' takes care of any symlink.
        if (os.path.dirname(IniFile.name) == ""):
            IniFile.name = os.path.join(os.path.dirname(os.path.realpath(sys.argv[0])), IniFile.name)
        self.open(IniFile.name)

        # Create any default tool table.
        self.auto_setup()

        grow = 0
        self.units = self.get_ini("TRAJ", "LINEAR_UNITS", default="inch")
        self.speed_button = tkinter.Label(self, text="Speed (%s/minute)" % (self.units))
        self.speed_button.grid(row=grow, column=1, sticky='e')
        self.speed_val = tkinter.StringVar()
        self.speed_val.set(self.get_ini("DISPLAY", "JOG_SPEED", default="6"))
        self.speed_entry = tkinter.Entry(self, textvariable=self.speed_val)  # use max z speed for default
        self.speed_entry.grid(row=grow, column=2, sticky='w')

        grow += 1
        self.inc_button = tkinter.Button(self, text="Incremental Jog (%s)" % (self.units),
                                         command=lambda: self.jog_type_button(JogTypeSel.INC))
        self.inc_button.grid(row=grow, column=1, sticky='news')
        self.inc_val = tkinter.StringVar()
        self.inc_val.set(self.get_ini("DISPLAY", "INC_JOG", default="0.2"))
        self.inc_entry = tkinter.Entry(self, textvariable=self.inc_val)
        self.inc_entry.grid(row=grow, column=2, sticky='w')

        self.spindle_val = tkinter.Label(self, text="Spindle (rpm)")
        self.spindle_val.grid(row=grow, column=3, columnspan=3)

        grow += 1
        self.abs_button = tkinter.Button(self, text="Absolute Jog (%s)" % (self.units),
                                          command=lambda: self.jog_type_button(JogTypeSel.ABS))
        self.abs_button.grid(row=grow, column=1, sticky='news')
        self.abs_val = tkinter.StringVar()
        self.abs_val.set(self.get_ini("DISPLAY", "ABS_JOG", default="1"))
        self.abs_entry = tkinter.Entry(self, textvariable=self.abs_val, state=tkinter.DISABLED)
        self.abs_entry.grid(row=grow, column=2, sticky='w')

        grow = 5
        self.mdi_button1 = tkinter.Button(self, text=self.get_ini("DISPLAY", "MDI_LABEL_1", default="MDI-1"),
                                          command=lambda: self.mdi(self.mdi_val1))
        self.mdi_button1.grid(row=grow, column=0, sticky='news')
        self.mdi_val1 = tkinter.StringVar()
        self.mdi_val1.set(self.get_ini("DISPLAY", "MDI_CMD_1", default=""))
        self.mdi_entry1 = tkinter.Entry(self, textvariable=self.mdi_val1)
        self.mdi_entry1.grid(row=grow, column=1, columnspan=5, sticky='ew')

        grow += 1
        self.mdi_button2 = tkinter.Button(self, text=self.get_ini("DISPLAY", "MDI_LABEL_2", default="MDI-2"),
                                          command=lambda: self.mdi(self.mdi_val2))
        self.mdi_button2.grid(row=grow, column=0, sticky='news')
        self.mdi_val2 = tkinter.StringVar()
        self.mdi_val2.set(self.get_ini("DISPLAY", "MDI_CMD_2", default=""))
        self.mdi_entry2 = tkinter.Entry(self, textvariable=self.mdi_val2)
        self.mdi_entry2.grid(row=grow, column=1, columnspan=5, sticky='ew')

        grow += 1
        self.mdi_button3 = tkinter.Button(self, text=self.get_ini("DISPLAY", "MDI_LABEL_3", default="MDI-3"),
                                          command=lambda: self.mdi(self.mdi_val3))
        self.mdi_button3.grid(row=grow, column=0, sticky='news')
        self.mdi_val3 = tkinter.StringVar()
        self.mdi_val3.set(self.get_ini("DISPLAY", "MDI_CMD_3", default=""))
        self.mdi_entry3 = tkinter.Entry(self, textvariable=self.mdi_val3)
        self.mdi_entry3.grid(row=grow, column=1, columnspan=5, sticky='ew')

        grow += 1
        self.mdi_button4 = tkinter.Button(self, text=self.get_ini("DISPLAY", "MDI_LABEL_4", default="MDI-4"),
                                          command=lambda: self.mdi(self.mdi_val4))
        self.mdi_button4.grid(row=grow, column=0, sticky='news')
        self.mdi_val4 = tkinter.StringVar()
        self.mdi_val4.set(self.get_ini("DISPLAY", "MDI_CMD_4", default=""))
        self.mdi_entry4 = tkinter.Entry(self, textvariable=self.mdi_val4)
        self.mdi_entry4.grid(row=grow, column=1, columnspan=5, sticky='ew')

        grow += 1
        self.auto_button = tkinter.Button(self, text="Auto", command=lambda: self.auto(AutoSel.OPEN))
        self.auto_button.grid(row=grow, column=0, sticky='news')
        self.auto_val = tkinter.StringVar()
        self.auto_val.set(self.get_ini("DISPLAY", "AUTO_FILE", default="your_file.nc"))
        self.auto_entry = tkinter.Entry(self, textvariable=self.auto_val)
        self.auto_entry.grid(row=grow, column=1, columnspan=3, sticky='ew')
        self.run_button = tkinter.Button(self, text="Run", command=lambda: self.auto(AutoSel.RUN))
        self.run_button.grid(row=grow, column=4, sticky='news')
        self.verify_button = tkinter.Button(self, text="Verify", command=lambda: self.auto(AutoSel.VERIFY))
        self.verify_button.grid(row=grow, column=5, sticky='news')

        grow += 1
        backrow = grow
        self.backplot_canvas = tkinter.Canvas(self, relief=tkinter.SUNKEN, borderwidth=4)
        self.backplot_canvas.grid(row=grow, column=0, columnspan=6, sticky='news')

        grow += 1
        self.zoom_in_button = tkinter.Button(self, text="Zoom In", command=lambda: self.bp3d.zoom_in())
        self.zoom_in_button.grid(row=grow, column=0, sticky='news')
        self.zoom_out_button = tkinter.Button(self, text="Zoom Out", command=lambda: self.bp3d.zoom_out())
        self.zoom_out_button.grid(row=grow, column=1, sticky='news')
        self.plot_3d_button = tkinter.Button(self, text="3D", command=lambda: self.bp3d.plot_3d())
        self.plot_3d_button.grid(row=grow, column=2, sticky='news')
        self.plot_xy_button = tkinter.Button(self, text="X - Y", command=lambda: self.bp3d.plot_xy())
        self.plot_xy_button.grid(row=grow, column=3, sticky='news')
        self.plot_xz_button = tkinter.Button(self, text="X - Z", command=lambda: self.bp3d.plot_xz())
        self.plot_xz_button.grid(row=grow, column=4, sticky='news')
        self.plot_yz_button = tkinter.Button(self, text="Y - Z", command=lambda: self.bp3d.plot_yz())
        self.plot_yz_button.grid(row=grow, column=5, sticky='news')

        #grow += 1

        # Create hidden tool table widget.
        tablefile = "%s/%s" % (HOME_DIR, self.get_ini("EMC", "TOOL_TABLE", default="stepper.tbl"))
        lathe = self.get_ini("DISPLAY", "LATHE", default="0") == "1"
        self.tool_table = tooltable.ToolTable(self, tablefile, lathe)

        # Create hidden fixture table widget.
        tablefile = "%s/%s" % (HOME_DIR, self.get_ini("EMC", "PARAMETER_TABLE", default="stepper.var"))
        self.fixture_table = fixture.FixtureTable(self, tablefile)

        self.columnconfigure(1, weight=1)
        self.columnconfigure(2, weight=1)
        self.rowconfigure(backrow, weight=1)
        self.rowconfigure(panelrow, weight=1)

    #=======================================================================
    def axis_button(self, id):
        if (id == AxisSel.X):
            self.x_val.config(relief=tkinter.GROOVE)
            self.y_val.config(relief=tkinter.FLAT)
            self.z_val.config(relief=tkinter.FLAT)
            self.a_val.config(relief=tkinter.FLAT)
            self.jogneg_button.config(text="Jog X -")
            self.jogpos_button.config(text="Jog X +")
        elif (id == AxisSel.Y):
            self.x_val.config(relief=tkinter.FLAT)
            self.y_val.config(relief=tkinter.GROOVE)
            self.z_val.config(relief=tkinter.FLAT)
            self.a_val.config(relief=tkinter.FLAT)
            self.jogneg_button.config(text="Jog Y -")
            self.jogpos_button.config(text="Jog Y +")
        elif (id == AxisSel.Z):
            self.x_val.config(relief=tkinter.FLAT)
            self.y_val.config(relief=tkinter.FLAT)
            self.z_val.config(relief=tkinter.GROOVE)
            self.a_val.config(relief=tkinter.FLAT)
            self.jogneg_button.config(text="Jog Z -")
            self.jogpos_button.config(text="Jog Z +")
        else:
            # TODO: display angular units instead of linear.
            self.x_val.config(relief=tkinter.FLAT)
            self.y_val.config(relief=tkinter.FLAT)
            self.z_val.config(relief=tkinter.FLAT)
            self.a_val.config(relief=tkinter.GROOVE)
            self.jogneg_button.config(text="Jog A -")
            self.jogpos_button.config(text="Jog A +")
        self.sel_axis = id

    #=======================================================================
    def jog_type_button(self, id):
        if (id == JogTypeSel.INC):
            self.inc_entry.config(state=tkinter.NORMAL)
            self.abs_entry.config(state=tkinter.DISABLED)
        else:
            self.inc_entry.config(state=tkinter.DISABLED)
            self.abs_entry.config(state=tkinter.NORMAL)
        self.sel_jog_type = id
        # Update jog buttons.
        self.axis_button(self.sel_axis)

    #=======================================================================
    def update_position(self, e):
        self.cur_pos = e['pos']
        self.x_val.config(text="X  %07.3f" % (self.cur_pos['x']))
        self.y_val.config(text="Y  %07.3f" % (self.cur_pos['y']))
        self.z_val.config(text="Z  %07.3f" % (self.cur_pos['z']))
        self.a_val.config(text="A  %07.3f" % (self.cur_pos['a']))

        if (e['line'] > 0):
            # Display gcode lines that have completed plus the next line.
            beg = self.line_num
            end = e['line']
            if (beg > 1):
                beg += 2
            if (end < self.line_max):
                end += 1

            self.line_num = e['line']  # save completed line

            # Display gcode lines in log_panel.
            line_txt = self.display_gcode_lines(self.auto_val.get(), beg, end, self.line_num)

            # Display completed line in backplot.
            self.bp3d.update_plot(line_txt, self.line_num, self.cur_pos)

            # Update status line count.
            self.status_line_num.config(text="%d/%d" % (self.line_num, self.line_max))

    #=======================================================================
    def update_rpm(self, hz):
        """Display spindle RPM."""
        self.spindle_val.config(text=("Spindle (rpm) %0.2f" % (hz*60))) # convert rps to rpm

    #=======================================================================
    def display_logger_message(self, e):
        numlines = self.log_panel.index('end - 1 line').split('.')[0]
        self.log_panel['state'] = 'normal'
        if (numlines == Panel.MAX_LINE):
            self.log_panel.delete(1.0, 2.0)
        if (self.log_panel.index('end-1c') != '1.0'):
            self.log_panel.insert('end', '\n')
            self.log_panel.see('end')
        self.log_panel.insert('end', e['msg'])
        self.log_panel['state'] = 'disabled'

    #=======================================================================
    def set_idle_state(self, flag):

        if (flag[0]):
            self.estop_button.config(state=tkinter.ACTIVE)
        else:
            self.estop_button.config(state=tkinter.DISABLED)

        if (flag[1]):
            self.home_button.config(state=tkinter.ACTIVE)
        else:
            self.home_button.config(state=tkinter.DISABLED)

        if (flag[2]):
            self.jogneg_button.config(state=tkinter.ACTIVE)
            self.jogpos_button.config(state=tkinter.ACTIVE)
        else:
            self.jogneg_button.config(state=tkinter.DISABLED)
            self.jogpos_button.config(state=tkinter.DISABLED)

        if (flag[3] == 1):
            self.run_button.config(state=tkinter.ACTIVE, text='Run')
        elif (flag[3] == 2):
            self.run_button.config(state=tkinter.ACTIVE, text='Cancel')
        else:
            self.run_button.config(state=tkinter.DISABLED, text='Run')

        if (flag[4]):
            self.mdi_button1.config(state=tkinter.ACTIVE)
            self.mdi_button2.config(state=tkinter.ACTIVE)
            self.mdi_button3.config(state=tkinter.ACTIVE)
            self.mdi_button4.config(state=tkinter.ACTIVE)
        else:
            self.mdi_button1.config(state=tkinter.DISABLED)
            self.mdi_button2.config(state=tkinter.DISABLED)
            self.mdi_button3.config(state=tkinter.DISABLED)
            self.mdi_button4.config(state=tkinter.DISABLED)

        if (flag[5] == 1):
            self.resume_button.config(state=tkinter.ACTIVE)
        else:
            self.resume_button.config(state=tkinter.DISABLED)

        if (flag[6] == 1):
            self.verify_button.config(state=tkinter.ACTIVE, text='Verify')
        elif (flag[6] == 2):
            self.verify_button.config(state=tkinter.ACTIVE, text='Cancel')
        else:
            self.verify_button.config(state=tkinter.DISABLED, text='Verify')

        if (flag[7] == 1):
            self.tool_table_button.config(state=tkinter.ACTIVE)
            self.tool_table.button_state(tkinter.ACTIVE)
        else:
            self.tool_table_button.config(state=tkinter.DISABLED)
            self.tool_table.button_state(tkinter.DISABLED)

        if (flag[8] == 1):
            self.fixture_button.config(state=tkinter.ACTIVE)
            self.fixture_table.button_state(tkinter.ACTIVE)
        else:
            self.fixture_button.config(state=tkinter.DISABLED)
            self.fixture_table.button_state(tkinter.DISABLED)

        if (self.dog.get_state() & MechStateBit.ESTOP):
            self.led_button.config(image=self.red_led)
            self.estop_button.config(text='EReset')
        elif ((self.dog.get_state() & MechStateBit.PAUSED) and (not flag[5] == 2)):
            self.led_button.config(image=self.orange_led)
        else:
            self.led_button.config(image=self.green_led)
            self.estop_button.config(text='EStop')

        if (self.dog.get_state() & MechStateBit.HOMED):
            self.x_val.config(fg="blue")
            self.y_val.config(fg="blue")
            self.z_val.config(fg="blue")
            self.a_val.config(fg="blue")
        else:
            self.x_val.config(fg="red")
            self.y_val.config(fg="red")
            self.z_val.config(fg="red")
            self.a_val.config(fg="red")

    #=======================================================================
    def jog(self, id):
        if (not self.safety_check_ok()):
            return

        self.set_idle_state(ButtonState.BUSY)
        m = {}
        m['id'] = MechEvent.CMD_MDI

        if (id == JogSel.NEG):
            # Jog negative.
            if (self.sel_jog_type == JogTypeSel.INC):
                # Perform incremental move. G91 will ignore any Z tool offset.
                val = abs(float(self.inc_val.get()))
                m['cmd'] = "G91 G1 %s%f F%s" % (axis_name[self.sel_axis], -val, self.speed_val.get())
            else:
                # Perform absolute move. G90 will use any Z tool offset.
                val = abs(float(self.abs_val.get()))
                m['cmd'] = "G90 G1 %s%f F%s" % (axis_name[self.sel_axis], -val, self.speed_val.get())
        else:
            # Jog positive.
            if (self.sel_jog_type == JogTypeSel.INC):
                # Perform incremental move.
                val = abs(float(self.inc_val.get()))
                m['cmd'] = "G91 G1 %s%f F%s" % (axis_name[self.sel_axis], val, self.speed_val.get())
            else:
                # perform absolute move
                val = abs(float(self.abs_val.get()))
                m['cmd'] = "G90 G1 %s%f F%s" % (axis_name[self.sel_axis], val, self.speed_val.get())

        self.mechq.put(m)

    #=======================================================================
    def mdi(self, id):
        if (not self.safety_check_ok()):
            return

        self.set_idle_state(ButtonState.BUSY)
        m = {}
        m['id'] = MechEvent.CMD_MDI
        m['cmd'] = id.get()
        self.mechq.put(m)

    #=======================================================================
    def set_all_zero(self):
        self.set_idle_state(ButtonState.BUSY)
        m = {}
        m['id'] = MechEvent.CMD_ALL_ZERO
        self.mechq.put(m)

    #=======================================================================
    def toggle_estop(self):
        self.set_idle_state(ButtonState.BUSY2)    # disable (block) estop button during a estop
        if (self.dog.get_state() & MechStateBit.ESTOP):
            m = {}
            m['id'] = MechEvent.CMD_ESTOP_RESET
            self.mechq.put(m)        # this command goes through the mech thread
        else:
            self.dog.estop()  # this command goes through the GUI thread.

    #=======================================================================
    def set_estop_state(self):
        if (self.dog.get_state() & MechStateBit.ESTOP):
            self.set_idle_state(ButtonState.ESTOP)
        else:
            self.set_idle_state(ButtonState.IDLE)

    #=======================================================================
    def safety_check_ok(self):
        if (self.dog.get_state() & MechStateBit.ESTOP):
            logging.error("Not out of ESTOP. Try pressing the ESTOP button.")
            return False

        if (not(self.dog.get_state() & MechStateBit.HOMED)):
            logging.warn("Not all zeroed. Try pressing the 'All Zero' button.")
            return False

        return True  # ok to run mech

    #=======================================================================
    def resume(self):
        self.set_idle_state(ButtonState.RESUME)
        m = {}
        m['id'] = MechEvent.CMD_RUN
        m['file'] = "paused"
        self.mechq.put(m)

    #=======================================================================
    def display_gcode_lines(self, gcodefile, beg, end, done):
        # Attempt to open the file.
        done_line = ""
        try:
            gfile = open(gcodefile,'r')
            # Count number of lines.
            cnt = 0
            line = gfile.readline()
            while (line != ''):
                cnt += 1
                if (cnt >= beg):
                    logging.info("%d: %s" % (cnt, line.rstrip()))
                if (cnt == done):
                    done_line = line.rstrip()  # save this line for backplot
                if (cnt == end):
                    break
                line = gfile.readline()
            gfile.close()
        except:
            pass
        return done_line

    #=======================================================================
    def display_max_line(self, gcodefile):
        # Attempt to open the file.
        try:
            gfile = open(gcodefile,'r')
            # Count number of lines.
            self.line_max = 0
            while (gfile.readline() != ''):
                self.line_max += 1
            self.line_num = 1
            self.status_line_num.config(text="%d/%d" % (self.line_num, self.line_max))
            gfile.close()
        except:
            pass

    #=======================================================================
    def auto(self, id):
        if (id == AutoSel.OPEN):
            gcodefile = filedialog.askopenfilename(parent=self, title="Open Gcode File")
            if (len(gcodefile) != 0):
                self.auto_val.set(gcodefile)
                self.display_max_line(gcodefile)
        elif (id == AutoSel.RUN):
            gcodefile = self.auto_val.get()
            if (gcodefile == ''):
                return
            if (not self.safety_check_ok()):
                return

            if (self.run_button.cget('text') == 'Cancel'):
                self.set_idle_state(ButtonState.BUSY)    # disable (block) cancel button during a cancel
                self.dog.auto_cancel_set()
                if (self.dog.get_state() & MechStateBit.PAUSED):
                    self.dog.paused_clear()
                    self.dog.auto_cancel_clear()
                    self.set_idle_state(ButtonState.IDLE)
            else:
                self.display_max_line(gcodefile)
                self.set_idle_state(ButtonState.AUTO)
                m = {}
                m['id'] = MechEvent.CMD_RUN
                m['file'] = gcodefile
                self.mechq.put(m)
                self.bp3d.clear_plot(self.cur_pos)
                self.tool_table.grid_remove()
                self.fixture_table.grid_remove()
        else:
            gcodefile = self.auto_val.get()
            if (gcodefile == ''):
                return
            if (self.verify_button.cget('text') == 'Cancel'):
                self.dog.verify_cancel_set()
            else:
                self.display_max_line(gcodefile)
                self.set_idle_state(ButtonState.VERIFY)
                m = {}
                m['id'] = MechEvent.CMD_VERIFY
                m['file'] = gcodefile
                self.mechq.put(m)
                self.bp3d.clear_plot(self.cur_pos)
                self.tool_table.grid_remove()
                self.fixture_table.grid_remove()

    #=======================================================================
    def edit_tool_table(self):
        """View/edit tool table."""
        self.fixture_table.grid_remove()
        self.tool_table.table_reload()
        self.tool_table.grid(row=10, column=0, columnspan=6, sticky='news')

    #=======================================================================
    def edit_fixture_table(self):
        """View/edit fixture table."""
        self.tool_table.grid_remove()
        self.fixture_table.table_reload()
        self.fixture_table.grid(row=10, column=0, columnspan=6, sticky='news')

    #=======================================================================
    def create_logger(self, panel):
        if (not os.path.exists(HOME_DIR)):
            # Create application '.' directory in user's home directory.
            os.makedirs(HOME_DIR)
        root = logging.getLogger()
        root.setLevel(level=logging.DEBUG)
        h = logging.handlers.TimedRotatingFileHandler("%s/%s" % (HOME_DIR, "log.txt"), "D", 1, backupCount=5)
        f = logging.Formatter('%(asctime)s:%(levelname)s:%(filename)s:%(lineno)s:%(message)s')
        h.setFormatter(f)
        root.addHandler(h)
        self.log_h = LogPanelHandler(panel)
        root.addHandler(self.log_h)
        logging.info("pymini %s %s" % (Version.release, Version.date))

    #=======================================================================
    def open(self, filename='rtstepper.ini'):
        logging.info("Loading configuration file: %s", filename)
        self.cfg = configparser.ConfigParser()
        dataset = self.cfg.read(filename)
        if (len(dataset) == 0):
            logging.error("Unable to load configuration file: %s" % (filename))
            raise Exception("Unable to load configuration file: %s" % (filename))
        logging.debug("TRAJ: %s %s %s %s %s %s" % (self.cfg.get("TRAJ", "LINEAR_UNITS"), 
            self.cfg.get("TRAJ", "ANGULAR_UNITS"), self.cfg.get("TRAJ", "DEFAULT_VELOCITY"),
            self.cfg.get("TRAJ", "MAX_VELOCITY"), self.cfg.get("TRAJ", "DEFAULT_ACCELERATION"),
            self.cfg.get("TRAJ", "MAX_ACCELERATION")))
        logging.debug("AXIS_0: %s %s %s %s %s %s %s %s %s %s %s %s %s" % (self.cfg.get("AXIS_0", "TYPE"),
            self.cfg.get("AXIS_0", "MAX_VELOCITY"), self.cfg.get("AXIS_0", "MAX_ACCELERATION"),
            self.cfg.get("AXIS_0", "BACKLASH"), self.cfg.get("AXIS_0", "INPUT_SCALE"),
            self.cfg.get("AXIS_0", "MIN_LIMIT"), self.cfg.get("AXIS_0", "MAX_LIMIT"),
            self.cfg.get("AXIS_0", "FERROR"), self.cfg.get("AXIS_0", "MIN_FERROR"),
            self.cfg.get("AXIS_0", "STEP_PIN"), self.cfg.get("AXIS_0", "DIRECTION_PIN"),
            self.cfg.get("AXIS_0", "STEP_ACTIVE_HIGH"), self.cfg.get("AXIS_0", "DIRECTION_ACTIVE_HIGH")))
        logging.debug("AXIS_1: %s %s %s %s %s %s %s %s %s %s %s %s %s" % (self.cfg.get("AXIS_1", "TYPE"),
            self.cfg.get("AXIS_1", "MAX_VELOCITY"), self.cfg.get("AXIS_1", "MAX_ACCELERATION"),
            self.cfg.get("AXIS_1", "BACKLASH"), self.cfg.get("AXIS_1", "INPUT_SCALE"),
            self.cfg.get("AXIS_1", "MIN_LIMIT"), self.cfg.get("AXIS_1", "MAX_LIMIT"),
            self.cfg.get("AXIS_1", "FERROR"), self.cfg.get("AXIS_1", "MIN_FERROR"),
            self.cfg.get("AXIS_1", "STEP_PIN"), self.cfg.get("AXIS_1", "DIRECTION_PIN"),
            self.cfg.get("AXIS_1", "STEP_ACTIVE_HIGH"), self.cfg.get("AXIS_1", "DIRECTION_ACTIVE_HIGH")))
        logging.debug("AXIS_2: %s %s %s %s %s %s %s %s %s %s %s %s %s" % (self.cfg.get("AXIS_2", "TYPE"),
            self.cfg.get("AXIS_2", "MAX_VELOCITY"), self.cfg.get("AXIS_2", "MAX_ACCELERATION"),
            self.cfg.get("AXIS_2", "BACKLASH"), self.cfg.get("AXIS_2", "INPUT_SCALE"),
            self.cfg.get("AXIS_2", "MIN_LIMIT"), self.cfg.get("AXIS_2", "MAX_LIMIT"),
            self.cfg.get("AXIS_2", "FERROR"), self.cfg.get("AXIS_2", "MIN_FERROR"),
            self.cfg.get("AXIS_2", "STEP_PIN"), self.cfg.get("AXIS_2", "DIRECTION_PIN"),
            self.cfg.get("AXIS_2", "STEP_ACTIVE_HIGH"), self.cfg.get("AXIS_2", "DIRECTION_ACTIVE_HIGH")))
        try:
            # Assume REV-3f FW or higher
            logging.debug("TASK: %s %s %s %s %s %s %s %s %s %s" % (self.cfg.get("TASK", "INPUT0_ABORT"),
                self.cfg.get("TASK", "INPUT1_ABORT"), self.cfg.get("TASK", "INPUT2_ABORT"),
                self.cfg.get("TASK", "INPUT3_ABORT"), self.cfg.get("TASK", "INPUT1_MODE"),
                self.cfg.get("TASK", "INPUT2_MODE"), self.cfg.get("TASK", "INPUT3_MODE"),
                self.cfg.get("TASK", "OUTPUT0_MODE"), self.cfg.get("TASK", "OUTPUT1_MODE"),
                self.cfg.get("TASK", "SERIAL_NUMBER")))
        except:
            logging.debug("TASK: %s %s %s %s" % (self.cfg.get("TASK", "INPUT0_ABORT"),
                self.cfg.get("TASK", "INPUT1_ABORT"), self.cfg.get("TASK", "INPUT2_ABORT"),
                self.cfg.get("TASK", "SERIAL_NUMBER")))


    #=======================================================================
    def close(self):
        self.dog.estop()
        self.dog.verify_cancel_set()
        root = logging.getLogger()
        root.removeHandler(self.log_h)
        self.mech.close()
        self.dog.close()

    #=======================================================================
    def auto_setup(self):
        """perform any initial application setup"""
        pass

#==========================================================================
bugout = False

try:
    opt, arg = getopt.getopt(sys.argv[1:], "i:h")
    for cmd, param in opt:
        if (cmd in ("-h")):
            usage()
            bugout = True
        if (cmd in ("-i")):
            IniFile.name = param
except:
    usage()
    bugout = True

if (not bugout):
    app = Gui()

    app.title("%s v%s" % (app.cfg.get("EMC", "MACHINE"), Version.release))
    app.icon = tkinter.PhotoImage(data=led.ICON)
    app.tk.call('wm', 'iconphoto', app._w, app.icon)
    try:
        app.mainloop()
    except KeyboardInterrupt:
        logging.error("User control-c...")
    app.close()
