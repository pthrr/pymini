#
# m192.py - m192 mcode script set/clear digital OUTPUT0-2.
#
# inputs:
#  p_num = pin number (0 = OUTPUT0, 1 = OUTPUT1, 2 = OUTPUT2)
#  q_num = set/clear (0 = clear, 1 = set)
#
# example:
#  m192 p0 q1 (set OUTPUT0)
#  m192 p0 q0 (clear OUTPUT0)
#  m192 p1 q1 (set OUTPUT1)
#  m192 p1 q0 (clear OUTPUT1)
#

import pyemc

def run(dongle, p_num, q_num):

    pin = int(p_num)
    value = int(q_num)

    if (pin < 0 or pin > 2):
        return pyemc.MechResult.EMC_R_ERROR # bail

    if (value < 0 or value > 1):
        return pyemc.MechResult.EMC_R_ERROR # bail

    if (value==0):
        dongle.dout_clear(pin)  # Set pin logic low
    else:
        dongle.dout_set(pin)  # Set pin logic high (~5vdc, ~25ma).
    return pyemc.MechResult.EMC_R_OK

if __name__ == "__main__":
   dog = pyemc.EmcMech()
   dog.open("", "rtstepper.ini")
   run(dog, 0, 0)
