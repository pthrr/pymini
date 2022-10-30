#
# m196.py - m196 mcode script set INPUT1-3 operating mode.
#
# inputs:
#  p_num = pin number (1 = INPUT1, 2 = INPUT2, 3 = INPUT3)
#  q_num = digital/ADC (0 = digital, 1 = ADC)
#
# notes:
#  Analog to Digital Conversion (ADC) 8-bit resolution, 16mhz / (64 * 4) = 62.5k clock.
#  Note, DB10-INPUT0 is digital only and cannot be changed.
#  
# example:
#  m196 p1 q0 (INPUT1 = digital mode)
#  m196 p1 q1 (INPUT1 = ADC mode)
#  m196 p2 q0 (INPUT2 = digital mode)
#  m196 p2 q1 (INPUt2 = ADC mode)
#  m196 p3 q0 (INPUT3 = digital mode)
#  m196 p3 q1 (INPUT3 = ADC mode)
#

import pyemc

def run(dongle, p_num, q_num):

    pin = int(p_num)
    value = int(q_num)

    if (pin < 1 or pin > 3):
        return pyemc.MechResult.EMC_R_ERROR # bail

    if (value < 0 or value > 1):
        return pyemc.MechResult.EMC_R_ERROR # bail

    dongle.input_mode(pin, value)

    return pyemc.MechResult.EMC_R_OK

if __name__ == "__main__":
   dog = pyemc.EmcMech()
   dog.open("", "rtstepper.ini")
   run(dog, 0, 0)
