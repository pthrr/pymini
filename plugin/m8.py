#
# m8.py - m8 mcode script turns coolant flood on.
# 
# inputs:
#  p_num = n/a
#  q_num = n/a
#
# example:
#  m8
#
import pyemc

# Change this define to True to set OUTPUT1.
M8_COOLANT_ON_ENABLE = False

def run(dongle, p_num, q_num):

    if (not M8_COOLANT_ON_ENABLE):
        return pyemc.MechResult.EMC_R_OK

    dongle.dout_set(pyemc.MechOutputNum.OUTPUT1)  # Set pin high (~5vdc, ~25ma).

    return pyemc.MechResult.EMC_R_OK
