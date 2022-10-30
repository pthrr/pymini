#
# m9.py - m9 mcode script turns coolant off.
# 
# inputs:
#  p_num = coolant  # 1 = mist, 2 = flood
#  q_num = n/a
#
# example:
#  m9
#
import pyemc

# Change this define to True to set OUTPUT1.
M9_COOLANT_OFF_ENABLE = False

def run(dongle, p_num, q_num):

    if (not M9_COOLANT_OFF_ENABLE):
        return pyemc.MechResult.EMC_R_OK

    dongle.dout_clear(pyemc.MechOutputNum.OUTPUT1)  # Set pin high (~5vdc, ~25ma).

    return pyemc.MechResult.EMC_R_OK
