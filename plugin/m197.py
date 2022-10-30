#
# m197.py - m197 mcode print INPUT1-3 ADC values.
#
# Notes:
#  Analog to Digital Conversion (ADC) 8-bit resolution, 16mhz / (64 * 4) = 62.5k clock.
#
# example:
#  m197
#

import pyemc, logging

def run(dongle, p_num, q_num):
    logging.info("M197 ADC values:")
    logging.info(" INPUT1=%d INPUT2=%d INPUT3=%d" % (dongle.input_adc_get(1), dongle.input_adc_get(2), dongle.input_adc_get(3)))
    return pyemc.MechResult.EMC_R_OK

if __name__ == "__main__":
   dog = pyemc.EmcMech()
   dog.open("", "rtstepper.ini")
   run(dog, 0, 0)
