#
# m190.py - m190 mcode script home specified axis using limit switch.
# 
#     WARNING!!! do NOT run this script without limits switches
#
# inputs:
#  p_num = din   # digital input number (0 = INPUT0, 1 = INPUT1, 2 = INPUT2)
#  q_num = axis  # axis number (0 = x, 1 = y, 2 = z)
#
# example:
# m190 p1 q0 (use INPUT1 for estop, home x-axis)
# m190 p1 q1 (use INPUT1 for estop, home y-axis)
# m190 p1 q2 (use INPUT1 for estop, home z-axis)
#
import pyemc, logging
from pyemc import MechStateBit
try:
   import configparser   # python3
except ImportError:
   import ConfigParser as configparser  # python2

din_map = ["INPUT0_ABORT", "INPUT1_ABORT", "INPUT2_ABORT"]
axis_section = ["AXIS_0", "AXIS_1", "AXIS_2"]
axis_letter = ["X", "Y", "Z"]
din_bit = [MechStateBit.INPUT0, MechStateBit.INPUT1, MechStateBit.INPUT2]

def run(dongle, p_num, q_num):

   din = int(p_num)
   axis = int(q_num)

   if (din < 0 or din > len(din_map)):
      logging.error("M190 error: invalid digital input %d" % (din))
      return pyemc.MechResult.EMC_R_ERROR # bail

   if (axis < 0 or axis > len(axis_section)):
      logging.error("M190 error: invalid axis %d" % (axis))
      return pyemc.MechResult.EMC_R_ERROR # bail

   ini_file = dongle.inifile

   cfg = configparser.ConfigParser()
   dataset = cfg.read(ini_file)
   if (len(dataset) == 0):
      logging.error("M190 error: unable to read %s" % (ini_file))
      return pyemc.MechResult.EMC_R_ERROR # bail, unable to load .ini file

   # Safety check, see if INPUTx will trigger estop in .ini file.
   key = din_map[din]
   val = cfg.get("TASK", key)
   if (val=="0"):
      logging.error("M190 error: %s not enabled in %s" % (key, ini_file))
      return pyemc.MechResult.EMC_R_ERROR # bail

   # Get soft limit for this axis from .ini file.
   try:
      dist = cfg.getfloat(axis_section[axis], "HOME_LIMIT")
   except:
      logging.error("M190 error: no valid HOME_LIMIT in %s" % (ini_file))
      return pyemc.MechResult.EMC_R_ERROR # bail

   # Get rapid feed rate for this axis from .ini file.
   try:
      speed = cfg.getfloat(axis_section[axis], "HOME_VELOCITY")
   except:
      logging.error("M190 error: no valid HOME_VELOCITY in %s" % (ini_file))
      return pyemc.MechResult.EMC_R_ERROR # bail

   # Convert units/second to units/minute. */
   speed = speed * 60

   # Move to the soft limit, assumes limit switch is before the soft limit.
   dongle.mdi_cmd("G1 %s%f F%f" % (axis_letter[axis], dist, speed))
   dongle.wait_io_done()

   # Make sure we hit estop.
   if (not(dongle.get_state() & MechStateBit.ESTOP)):
      logging.error("M190 error: no ESTOP")
      return pyemc.MechResult.EMC_R_ERROR # bail

   # Make sure INPUTx bit is set (1).
   if (not(dongle.get_state() & din_bit[din])):
      logging.error("M190 error: no digital input %d" % (din))
      return pyemc.MechResult.EMC_R_ERROR # bail

   # Disable INPUTx estop trigger.
   dongle.din_abort_disable(din)

   # Reset estop.
   dongle.estop_reset()

   # Zero the axis.
   dongle.home()

   # Move to "home" position specified in .ini file for this axis.
   dist = cfg.getfloat(axis_section[axis], "HOME")
   dongle.mdi_cmd("G1 %s%f F%f" % (axis_letter[axis], dist, speed))
   dongle.wait_io_done()

   # Zero the axis.
   dongle.home()

   # Enable INPUTx estop trigger.
   dongle.din_abort_enable(din)

   return pyemc.MechResult.EMC_R_OK

if __name__ == "__main__":
   dog = pyemc.EmcMech()
   run(dog, 0, 0)
