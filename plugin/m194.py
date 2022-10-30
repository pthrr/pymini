#
# m194.py - m194 mcode script set OUTPUT0-1 operating mode.
#
# inputs:
#  p_num = pin number (0 = OUTPUT0, 1 = OUTPUT1)
#  q_num = digital/PWM (0 = digital, 1 = PWM)
#
# example:
#  m194 p0 q0 (OUTPUT0 = digital mode)
#  m194 p0 q1 (OUTPUT0 = PWM mode inverted)
#  m194 p1 q0 (OUTPUT1 = digital mode)
#  m194 p1 q1 (OUTPUT1 = PWM mode non-inverted)
#

import pyemc

def run(dongle, p_num, q_num):

    pin = int(p_num)
    value = int(q_num)

    if (pin < 0 or pin > 1):
        return pyemc.MechResult.EMC_R_ERROR # bail

    if (value < 0 or value > 1):
        return pyemc.MechResult.EMC_R_ERROR # bail

    dongle.output_mode(pin, value)

    return pyemc.MechResult.EMC_R_OK

if __name__ == "__main__":
   dog = pyemc.EmcMech()
   dog.open("", "rtstepper.ini")
   run(dog, 0, 0)
