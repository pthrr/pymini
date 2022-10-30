#
# m5.py - m5 mcode script turns spindle off using PWM.
# 
# inputs:
#  p_num = n/a
#  q_num = n/a
#
# assumptions:
#  dongle firmware = REV-3f or later
#  OUTPUT1_MODE = PWM (ie: set .ini file OUTPUT1_MODE = 1)
#
# example:
#  m5
#
import pyemc

# Change this define to True to set OUTPUT0.
M5_SPINDLE_OFF_ENABLE = False

def run(dongle, p_num, q_num):

    if (not M5_SPINDLE_OFF_ENABLE):
        return pyemc.MechResult.EMC_R_OK

    # Set the PWM duty cycle, 0 = 0v.
    dongle.output_pwm(pyemc.MechOutputNum.OUTPUT1, int(0))

    return pyemc.MechResult.EMC_R_OK

if __name__ == "__main__":
    dog = pyemc.EmcMech()
    run(dog, 0, 0)

