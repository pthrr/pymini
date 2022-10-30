#
# m4.py - m4 mcode script turns spindle on counter clockwise using PWM.
# 
# inputs:
#  p_num = rpm   # S parameter (spindle speed)
#  q_num = n/a
#
# assumptions:
#  dongle firmware = REV-3f or later
#  OUTPUT1_MODE = PWM (ie: set .ini file OUTPUT1_MODE = 1)
#
# example:
#  m4 s250.0
#
import pyemc, logging

# Change this define to True to set OUTPUT1.
M4_SPINDLE_ON_ENABLE = False

def run(dongle, p_num, q_num):

    if (not M4_SPINDLE_ON_ENABLE):
        return pyemc.MechResult.EMC_R_OK

    # Enable spindle counter clockwise rotation. 
    dongle.dout_set(pyemc.MechOutputNum.OUTPUT2)  # Set pin logic high
    #dongle.dout_clear(pyemc.MechOutputNum.OUTPUT2)  # Set pin logic low

    # Convert S parameter to 0-255 duty cycle, assumes max spindle speed is 6000 rpm. 
    value = p_num * 0.0425   # 255 / 6000 = 0.0425

    # Set the PWM duty cycle.
    dongle.output_pwm(pyemc.MechOutputNum.OUTPUT1, int(value))

    return pyemc.MechResult.EMC_R_OK

if __name__ == "__main__":
    dog = pyemc.EmcMech()
    run(dog, 0, 0)
