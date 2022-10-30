#!/usr/bin/python
# test_cancel.py - A command line test script. Tests pymini cancel command.
#
# (c) 2014-2017 Copyright Eckler Software
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

import os, sys, getopt, logging, threading, time, random, filecmp, datetime
try:
    import queue
except ImportError:
    import Queue as queue
import pyemc
from pyemc import MechStateBit, MechResult
from version import Version

HOME_DIR = "%s/.%s" % (os.path.expanduser("~"), Version.name)

EPSILON = 0.0000001

class GuiEvent(object):
    """Events from mech to gui."""
    MECH_IDLE = 1    # na 
    LOG_MSG = 2      # na 
    MECH_DEFAULT = 3  # unused
    MECH_POSITION = 4  # position update from mech
    MECH_ESTOP = 5  # auto estop from mech
    MECH_PAUSED = 6  # M0, M1 or M60 from parser

#=======================================================================
class Mech(object):
    def __init__(self, dog, gfile):
        self.tid = self.mech_thread(dog, gfile)
        self.tid.start()

    class mech_thread(threading.Thread):
        def __init__(self, dog, gfile):
            threading.Thread.__init__(self)
            self.dog = dog
            self.gfile = gfile

        # Main thread() function.
        def run(self):
            if (not(self.dog.get_state() & MechStateBit.ESTOP)):
                self.dog.auto_cmd(self.gfile)
                self.dog.wait_io_done()
            logging.info("Mech thread closing...")

    def close(self):
        self.tid.join()
        self.tid = None

#=======================================================================
class MechEvent(object):
    def __init__(self, guiq, outfile):
        self.tid = self.event_thread(guiq, outfile)
        self.tid.start()

    class event_thread(threading.Thread):
        def __init__(self, guiq, outfile):
            threading.Thread.__init__(self)
            self.done = threading.Event()
            self.guiq = guiq
            self.outfile = outfile
            self.first = True
            self.last_line = 0
            self.cur_pos = {'x': 0.0, 'y': 0.0, 'z': 0.0}
            self.first_pos = {'x': 0.0, 'y': 0.0, 'z': 0.0}

        def shutdown(self):
            self.done.set()  # stop thread()
        
        # Main thread() function.
        def run(self):
            """ Check queue for any io initiated events. """
            with open(self.outfile, 'w') as f:
                while (1):            
                    # Check for thread shutdown.
                    if (self.done.isSet()):
                        break

                    time.sleep(0.2)   # wait 200ms

                    while (not self.guiq.empty()):
                        e = self.guiq.get()
                        if (e['id'] == GuiEvent.MECH_POSITION):
                            if (e['line'] == 0):
                                continue  # ignore mdi commands
                            self.cur_pos = e['pos']
                            self.last_line = e['line']
                            if (self.first):
                                self.first_pos = self.cur_pos
                                self.first = False
                            f.write("%d: x=%0.5f y=%0.5f z=%0.5f\n" % (e['line'], self.cur_pos['x'], self.cur_pos['y'], self.cur_pos['z']))
                            f.flush()
                        elif (e['id'] == GuiEvent.MECH_ESTOP):
                            logging.info("mech estop...")  # auto estop from mech
                        elif (e['id'] == GuiEvent.MECH_PAUSED):
                            logging.info("mech paused...")  # auto pause from parser
                        else:
                            logging.info("unable to process gui event %d" % (e['id']))
                        e = None

    def last_line(self):
        return self.tid.last_line

    def last_posistion(self):
        return self.tid.cur_pos

    def first_posistion(self):
        return self.tid.first_pos

    def close(self):
        logging.info("Event thread closing...")
        self.tid.shutdown()
        self.tid.join()
        self.tid = None

def line_parse(line):
    """Given the gcode line, parse out position for each axis."""
    pos = {}
    ln = line.split()
    num = int(ln[0].split(':')[0])
    pos['x'] = float(ln[1].split('=')[1])
    pos['y'] = float(ln[2].split('=')[1])
    pos['z'] = float(ln[3].split('=')[1])
    return num, pos

def gfile_parse(gfile, line_num):
    """Given the gcode file, determine which axis is active for this line number."""
    x=None
    y=None
    z=None
    with open(gfile) as f:
        line = f.readline()
        cnt = 0
        while (line != ''):
            cnt += 1
            if (cnt == line_num):
                line = line.lower()
                ln = line.split()
                for i in range(1, len(ln)):
                    if ('(' == ln[i][0]):
                        break  # comment, done
                    elif ('x' == ln[i][0]):
                        x = float(ln[i][1:])
                    elif ('y' == ln[i][0]):
                        y = float(ln[i][1:])
                    elif ('z' == ln[i][0]):
                        z = float(ln[i][1:])
                break
            line = f.readline()
    return x, y, z

def isclose(a, b, rel_tol=1e-09, abs_tol=0.0):
    return abs(a-b) < EPSILON

def tmstamp():
    return datetime.datetime.fromtimestamp(time.time()).strftime('%Y-%m-%d %H:%M:%S')

#=======================================================================
def usage():
   print("test_cancel %s, rt-stepper dongle cancel test" % (Version.release))
   print("(c) 2013-2017 Copyright Eckler Software")
   print("David Suffield, dsuffiel@ecklersoft.com")
   print("usage: test_cancel [-i inifile] -f gcode.nc [-s serial_number] [-p loop_count]")

#=======================================================================
logging.basicConfig(filename='test.log', level=logging.DEBUG, format='%(asctime)s:%(levelname)s:%(filename)s:%(lineno)s:%(message)s')

snum = ""
ini = "rtstepper.ini"
gfile = "cutout2.nc"
loopcnt = 1
try:
   opt, arg = getopt.getopt(sys.argv[1:], "s:f:i:p:h")
   for cmd, param in opt:
      if (cmd in ("-h")):
          usage()
          sys.exit(0)
      if (cmd in ("-s")):
          snum = param
      if (cmd in ("-f")):
          gfile = param
      if (cmd in ("-i")):
          ini = param
      if (cmd in ("-p")):
          loopcnt = int(param)

except SystemExit:
    sys.exit(0)
except:
    usage()
    sys.exit(1)

if (not os.path.isfile(gfile)):
    print("Unable to open gcode file: %s" % (gfile))
    sys.exit(1)

if (not os.path.isfile(ini)):
    print("Unable to open .ini file: %s" % (ini))
    sys.exit(1)
    
# Instantiate dongle.
dog = pyemc.EmcMech()

# Display dll version.
print("Opened %s %s" % (dog.LIBRARY_FILE, dog.get_version()))

guiq = queue.Queue()  # Mech to ui communication

dog.open(HOME_DIR, ini)

if (dog.get_state() & MechStateBit.ESTOP):
    sys.exit()  # no dongle found

dog.register_logger_cb()
dog.register_event_cb(guiq)

# Run gcode and create expected output file.
expfile = "%s.exp" % (gfile)
event = MechEvent(guiq, expfile)  # kickoff event thread
print("%s Creating expected output with %s" % (tmstamp(), gfile))
stm = time.time()  # measure execution time
mech = Mech(dog, gfile)  # kickoff mech thread
mech.close()       # wait for mech thread to close
tm = time.time() - stm  # execution time in seconds
time.sleep(0.3)  # wait for event_thread to catch up
first_pos = event.first_posistion()
last_line = event.last_line()
last_pos = event.last_posistion()
event.close()  # wait for event_thread to close
print("%s Finished output in %f minutes" % (tmstamp(), tm/60.0))
maxcnt = loopcnt

while (loopcnt):
    loopcnt -= 1
    print("%s Pass %d/%d" % (tmstamp(), maxcnt-loopcnt, maxcnt))

    # Run gcode with random cancel.
    cfile = "%s.cancel" % (gfile)
    event = MechEvent(guiq, cfile)  # kickoff event thread
    print("%s Creating %s output with random cancel" % (tmstamp(), gfile))
    mech = Mech(dog, gfile)  # kickoff mech thread
    time.sleep(random.uniform(0.1, tm))
    dog.auto_cancel_set()  # start synchronized cancel
    mech.close()
    time.sleep(0.3)  # wait for event_thread to catch up
    last_cancel_line = event.last_line()
    if (last_cancel_line == 0):
        last_cancel_pos = last_pos  # cancel occured before any move was started
    else:        
        last_cancel_pos = event.last_posistion()
    event.close()
    print("%s Canceled after line number %d/%d" % (tmstamp(), last_cancel_line, last_line))

    # Clean up after cancel, set interpreter to last known position.
    dog.auto_cancel_clear()
    dog.set_position(dog.get_position())

    # Verify cancel file is subset of expected.
    with open(cfile, 'r') as f1:
        with open(expfile, 'r') as f2:
            first_x = first_y = first_z = True
            act = f1.readline()
            exp = f2.readline()
            while (act != ''):
                exp_line, exp_pos = line_parse(exp)
                act_line, act_pos = line_parse(act)
                x, y, z = gfile_parse(gfile, exp_line)
                if (exp_line != act_line):
                    # Line numbers do not match, check if act line was optimized out by interpreter.
                    bump = False
                    if (x != None and first_x):
                        if (isclose(exp_pos['x'], last_pos['x'])):
                            bump = True
                            first_x = False
                    if (y != None and first_y):
                        if (isclose(exp_pos['y'], last_pos['y'])):
                            bump = True
                            first_y = False
                    if (z != None and first_z):
                        if (isclose(exp_pos['z'], last_pos['z'])):
                            bump = True
                            first_z = False
                    if (bump):
                        exp = f2.readline() # bump to next exp line
                        continue
                    print("Error cancel line not equal exp=%s act=%s" % (exp, act))
                    sys.exit(1)
                else:  # if (exp_line == act_line):
                    if (x != None):
                        if (not(isclose(exp_pos['x'], act_pos['x']))):
                            print("Error cancel line not equal exp=%s act=%s" % (exp, act))
                            sys.exit(1)
                    if (y != None):
                        if (not(isclose(exp_pos['y'], act_pos['y']))):
                            print("Error cancel line not equal exp=%s act=%s" % (exp, act))
                            sys.exit(1)
                    if (z != None):
                        if (not(isclose(exp_pos['z'], act_pos['z']))):
                            print("Error cancel line not equal exp=%s act=%s" % (exp, act))
                            sys.exit(1)
                act = f1.readline()
                exp = f2.readline()

    # Run gcode and save actual output.
    actfile = "%s.act" % (gfile)
    event = MechEvent(guiq, actfile)  # kickoff event thread
    print("%s Creating actual output with %s" % (tmstamp(), gfile))
    mech = Mech(dog, gfile)  # kickoff mech thread
    mech.close()
    event.close()

    # Verify actual file = expected file.
    with open(actfile, 'r') as f1:
        with open(expfile, 'r') as f2:
            first_x = first_y = first_z = True
            act = f1.readline()
            exp = f2.readline()
            while (act != ''):
                exp_line, exp_pos = line_parse(exp)
                act_line, act_pos = line_parse(act)
                x, y, z = gfile_parse(gfile, exp_line)
                if (exp_line != act_line):
                    # Line numbers do not match, check if act line was optimized out by interpreter.
                    bump = False
                    if (x != None and first_x):
                        if (isclose(exp_pos['x'], last_cancel_pos['x'])):
                            bump = True
                            first_x = False
                    if (y != None and first_y):
                        if (isclose(exp_pos['y'], last_cancel_pos['y'])):
                            bump = True
                            first_y = False
                    if (z != None and first_z):
                        if (isclose(exp_pos['z'], last_cancel_pos['z'])):
                            bump = True
                            first_z = False
                    if (bump):
                        exp = f2.readline() # bump to next line
                        continue
                    print("Error actual line not equal exp=%s act=%s" % (exp, act))
                    sys.exit(1)
                else:  # if (exp_line == act_line):
                    if (x != None):
                        if (not(isclose(exp_pos['x'], act_pos['x']))):
                            print("Error actual line not equal exp=%s act=%s" % (exp, act))
                            sys.exit(1)
                    if (y != None):
                        if (not(isclose(exp_pos['y'], act_pos['y']))):
                            print("Error actual line not equal exp=%s act=%s" % (exp, act))
                            sys.exit(1)
                    if (z != None):
                        if (not(isclose(exp_pos['z'], act_pos['z']))):
                            print("Error actual line not equal exp=%s act=%s" % (exp, act))
                            sys.exit(1)
                act = f1.readline()
                exp = f2.readline()
  
print("Test passed.")
dog.close()