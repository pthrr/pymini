#
# m193.py - m193 mcode script print runtime stats.
#
# example:
#  m193 
#

import pyemc, logging

def run(dongle, p_num, q_num):
    logging.info("M193 runtime stats:")
    logging.info(" current rpm=%0.2f rpm_avg=%0.2f" % (dongle.din_frequency_get(0)*60, dongle.din_frequency_avg_get(0)*60))
    logging.info(" spindle synchronized rpm_max=%0.2f rpm_min=%0.2f" % (dongle.din_frequency_max_get(0)*60, dongle.din_frequency_min_get(0)*60))
    return pyemc.MechResult.EMC_R_OK

if __name__ == "__main__":
   dog = pyemc.EmcMech()
   dog.open("", "rtstepper.ini")
   run(dog, 0, 0)
