#
# m6.py - m6 mcode script tool change.
# 
# inputs:
#  p_num = tool number
#  q_num = n/a
#
# return value:
#  EMC_R_OK = no program pause
#  EMC_R_PROGRAM_PAUSED = enable program pause (user clicks Resume button to continue)
#
# example:
#  M6 T1
#
import pyemc

def run(dongle, p_num, q_num):
   #return pyemc.MechResult.EMC_R_OK
   return pyemc.MechResult.EMC_R_PROGRAM_PAUSED

if __name__ == "__main__":
   dog = pyemc.EmcMech()
   run(dog, 0, 0)

