#
# m195.py - m195 mcode script set OUTPUT0-1 PWM duty cycle.
#
# inputs:
#  p_num = pin number (0 = OUTPUT0, 1 = OUTPUT1)
#  q_num = PWM (0-255)
#
# Notes:
#  OUTPUT0 is inverted
#  OUTPUT1 is non-inverted
#  PWM frequency is 976.5625hz
#
# example:
#  m195 p0 q0 (OUTPUT0 = 0% duty cycle, logic high)
#  m195 p0 q255 (OUTPUT0 = 100% duty cycle, logic low)
#  m195 p0 q85 (OUTPUT0 = 30% duty cycle)
#  m195 p1 q0 (OUTPUT1 = 0% duty cycle, logic low)
#  m195 p1 q255 (OUTPUT1 = 100% duty cycle, logic high)
#  m195 p1 q85 (OUTPUT1 = 30% duty cycle)
#

import pyemc, logging

def run(dongle, p_num, q_num):

    pin = int(p_num)
    value = int(q_num)

    if (pin < 0 or pin > 1):
        return pyemc.MechResult.EMC_R_ERROR # bail

    if (value < 0 or value > 255):
        return pyemc.MechResult.EMC_R_ERROR # bail

    dongle.output_pwm(pin, value)

    return pyemc.MechResult.EMC_R_OK

if __name__ == "__main__":
   dog = pyemc.EmcMech()
   dog.open("", "rtstepper.ini")
   run(dog, 0, 0)
