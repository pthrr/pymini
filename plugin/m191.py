#
# m191.py - m191 mcode script set position for specified axis.
# 
# inputs:
#  p_num = axis  # axis number (0 = x, 1 = y, 2 = z)
#  q_num = position
#
# example:
#  m191 p0 q0 (x=0)
#
import pyemc

axis_letter = ["x", "y", "z", "a", "b", "c", "u", "v", "w"]

def run(dongle, p_num, q_num):

   axis = int(p_num)
   position = q_num

   if (axis < 0 or axis > len(axis_letter)-1):
      return pyemc.MechResult.EMC_R_ERROR # bail

   # First get dictionary of all current positions for each axis.
   old = dongle.get_position()
   # Set the position for the specified axis. 
   old[axis_letter[axis]] = position  
   # Write dictionary with the new position value.
   dongle.set_position(old)
   return pyemc.MechResult.EMC_R_OK

if __name__ == "__main__":
   dog = pyemc.EmcMech()
   dog.open("", "rtstepper.ini")
   run(dog, 0, 0)
