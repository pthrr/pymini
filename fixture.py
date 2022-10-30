#!/usr/bin/python
# fixture.py - fixture offset table widget. Used by pymini.py.
#
# (c) 2014-2017 Copyright Eckler Software
#
# Author: David Suffield, dsuffiel@ecklersoft.com
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of version 2 of the GNU General Public License as published by
# the Free Software Foundation.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
#
# Upstream patches are welcome. Any patches submitted to the author must be
# unencumbered (ie: no Copyright or License).
#

import os, sys, logging
try:
    import tkinter
except ImportError:
    import Tkinter as tkinter

_required_parameters = [ 5161, 5162, 5163,   # G28 home 
 5164, 5165, 5166, # A, B, & C 
 5167, 5168, 5169, # U, V, & W 
 5181, 5182, 5183,   # G30 home 
 5184, 5185, 5186, # A, B, & C 
 5187, 5188, 5189, # U, V, & W 
 5210, # G92 is currently applied 
 5211, 5212, 5213,   # G92 offsets 
 5214, 5215, 5216, # A, B, & C 
 5217, 5218, 5219, # U, V, & W 
 5220,               # selected coordinate 
 5221, 5222, 5223,   # coordinate system 1 
 5224, 5225, 5226, # A, B, & C 
 5227, 5228, 5229, # U, V, & W 
 5230,
 5241, 5242, 5243,   # coordinate system 2 
 5244, 5245, 5246, # A, B, & C 
 5247, 5248, 5249, # U, V, & W 
 5250,
 5261, 5262, 5263,   # coordinate system 3 
 5264, 5265, 5266, # A, B, & C 
 5267, 5268, 5269, # U, V, & W 
 5270,
 5281, 5282, 5283,   # coordinate system 4 
 5284, 5285, 5286, # A, B, & C 
 5287, 5288, 5289, # U, V, & W 
 5290,
 5301, 5302, 5303,   # coordinate system 5 
 5304, 5305, 5306, # A, B, & C 
 5307, 5308, 5309, # U, V, & W 
 5310,
 5321, 5322, 5323,   # coordinate system 6 
 5324, 5325, 5326, # A, B, & C 
 5327, 5328, 5329, # U, V, & W 
 5330,
 5341, 5342, 5343,   # coordinate system 7 
 5344, 5345, 5346, # A, B, & C 
 5347, 5348, 5349, # U, V, & W 
 5350,
 5361, 5362, 5363,   # coordinate system 8 
 5364, 5365, 5366, # A, B, & C 
 5367, 5368, 5369, # U, V, & W 
 5370,
 5381, 5382, 5383,   # coordinate system 9 
 5384, 5385, 5386, # A, B, & C 
 5387, 5388, 5389, # U, V, & W 
 5390 ]

axis_name = ["X", "Y", "Z", "A"]  # axis to motor map
axis_letter = ["x", "y", "z", "a"]

gcode_coordinate = ["G54","G55","G56","G57","G58","G59","G59.1","G59.2","G59.3"] # gcode coordinate
gcode_coordinate_var = ["5221","5241","5261","5281","5301","5321","5341","5361","5381"]  # gcode to var map
touch_off_radius_var = "5218"
touch_off_length_var = "5219"

class GcodeSel(object):
    G54 = 0
    G55 = 1
    G56 = 2
    G57 = 3
    G58 = 4
    G59 = 5
    G59_1 = 6
    G59_2 = 7
    G59_3 = 8

class FixtureTable(tkinter.Frame):
    """FixtureTable widget"""
    def __init__(self, parent=None, fixture_file=""):
        tkinter.Frame.__init__(self, parent)
        self.fixture_file = fixture_file
        self.parent = parent
        self.result = None

        self.table = self.table_read()

        grow = 1
        self.m_label = []
        self.m_val = []
        self.m_entry = []
        self.m_button = []
        for i in range(len(axis_name)):
           self.m_label.append(tkinter.Label(self, text="%s " % (axis_name[i])))
           self.m_label[i].grid(row=grow, column=1, sticky='e')
           self.m_val.append(tkinter.DoubleVar())
           self.m_entry.append(tkinter.Entry(self, width=10, textvariable=self.m_val[i]))
           self.m_entry[i].grid(row=grow, column=2, sticky='ew')
           self.m_button.append(tkinter.Button(self, text="Teach", command=lambda i=i: self.teach(i)))
           self.m_button[i].grid(row=grow, column=3)
           grow += 1

        self.gcode_sel = tkinter.IntVar()
        self.gcode_sel.set(GcodeSel.G54)
        self.gcode_sel_old = self.gcode_sel.get()

        grow = 0
        self.g54_button = tkinter.Radiobutton(self, text=gcode_coordinate[GcodeSel.G54], variable=self.gcode_sel, value=GcodeSel.G54, command=self.radio_sel)
        self.g54_button.grid(row=grow, column=0, sticky='w')
        grow += 1
        self.g55_button = tkinter.Radiobutton(self, text=gcode_coordinate[GcodeSel.G55], variable=self.gcode_sel, value=GcodeSel.G55, command=self.radio_sel)
        self.g55_button.grid(row=grow, column=0, sticky='w')
        grow += 1
        self.g56_button = tkinter.Radiobutton(self, text=gcode_coordinate[GcodeSel.G56], variable=self.gcode_sel, value=GcodeSel.G56, command=self.radio_sel)
        self.g56_button.grid(row=grow, column=0, sticky='w')
        grow += 1
        self.g57_button = tkinter.Radiobutton(self, text=gcode_coordinate[GcodeSel.G57], variable=self.gcode_sel, value=GcodeSel.G57, command=self.radio_sel)
        self.g57_button.grid(row=grow, column=0, sticky='w')
        grow += 1
        self.g58_button = tkinter.Radiobutton(self, text=gcode_coordinate[GcodeSel.G58], variable=self.gcode_sel, value=GcodeSel.G58, command=self.radio_sel)
        self.g58_button.grid(row=grow, column=0, sticky='w')
        grow += 1
        self.g59_button = tkinter.Radiobutton(self, text=gcode_coordinate[GcodeSel.G59], variable=self.gcode_sel, value=GcodeSel.G59, command=self.radio_sel)
        self.g59_button.grid(row=grow, column=0, sticky='w')
        grow += 1
        self.g59_1_button = tkinter.Radiobutton(self, text=gcode_coordinate[GcodeSel.G59_1], variable=self.gcode_sel, value=GcodeSel.G59_1, command=self.radio_sel)
        self.g59_1_button.grid(row=grow, column=0, sticky='w')
        grow += 1
        self.g59_2_button = tkinter.Radiobutton(self, text=gcode_coordinate[GcodeSel.G59_2], variable=self.gcode_sel, value=GcodeSel.G59_2, command=self.radio_sel)
        self.g59_2_button.grid(row=grow, column=0, sticky='w')
        grow += 1
        self.g59_3_button = tkinter.Radiobutton(self, text=gcode_coordinate[GcodeSel.G59_3], variable=self.gcode_sel, value=GcodeSel.G59_3, command=self.radio_sel)
        self.g59_3_button.grid(row=grow, column=0, sticky='w')

        self.zero_button = tkinter.Button(self, text="Zero G54", command=self.zero)
        self.zero_button.grid(row=grow, column=1)
        self.ok_button = tkinter.Button(self, text="OK", command=self.ok)
        self.ok_button.grid(row=grow, column=2)
        self.cancel_button = tkinter.Button(self, text="Cancel", command=self.cancel)
        self.cancel_button.grid(row=grow, column=3)

        # Update table values.
        self.update(self.gcode_sel_old)

    def save(self, gid):
        """Write dialog entry into dictionary."""
        var = int(gcode_coordinate_var[gid])
        for i in range(len(axis_name)):
            self.table["%s" % (var+i)] = self.m_val[i].get()

    def update(self, gid):
        """Write dictionary entry into dialog."""
        var = int(gcode_coordinate_var[gid])
        for i in range(len(axis_name)):
            self.m_val[i].set(self.table["%s" % (var+i)])

    def zero(self):
        """Zero each axis for current selection."""
        for i in range(len(axis_name)):
            self.m_val[i].set(0.0)

    def teach(self, axis):
        """Set axis value to the current position."""
        pos = self.parent.dog.get_position()
        self.m_val[axis].set(pos[axis_letter[axis]])

    def radio_sel(self):
        """Select different gcode offset."""
        try:
            self.save(self.gcode_sel_old)
        except Exception as err:
            logging.error("Input error: %s" % (err))
            self.gcode_sel.set(self.gcode_sel_old)
            return  # leave dialog here, no selection change
        self.update(self.gcode_sel.get())
        self.gcode_sel_old = self.gcode_sel.get()
        self.zero_button.config(text="Zero %s" % (gcode_coordinate[self.gcode_sel.get()]))

    def ok(self, event=None):
        try:
            self.save(self.gcode_sel_old)
        except Exception as err:
            logging.error("Unable save fixture offsets: %s" % (err))
            return  # leave dialog open
        
        self.result = self.table

        # Close old stepper.var file.
        self.parent.dog.close()
        try:
            # Write new stepper.var file.
            self.table_write(self.table)
            logging.info("Saved fixture offsets: %s" % (self.fixture_file))            
            # Open new stepper.var file.
            self.parent.dog.open(self.parent.homedir, self.parent.inifile)           
        except Exception as err:
            logging.error("Unable save fixture offsets: %s" % (err))
            return # leave dialog open
        self.cancel()

    def table_read(self):
        """Create a dictionary of all stepper.var file entries."""
        tbl = {}
        self.default_write()
        if (not os.path.exists(self.fixture_file)):
            logging.error("Unable to load var file: %s" % (self.fixture_file))
            return tbl
        with open(self.fixture_file, 'r') as f:
            line = f.readline()
            while (line != ''):
                ln = line.split()
                if (len(ln) != 2):
                    line = f.readline()
                    continue
                tbl[ln[0]] = float(ln[1])
                line = f.readline()
        return tbl

    def table_write(self, tbl):
        # Backup the old stepper.var file.
        if (os.path.exists(self.fixture_file)):
            backup = "%s.bak" % (self.fixture_file)
            if (os.path.exists(backup)):
                os.remove(backup)
            os.rename(self.fixture_file, backup)

        with open(self.fixture_file, 'w') as f:
            for key in sorted(tbl):
                f.write("%s %g\n" % (key, tbl[key]))

    def cancel(self, event=None):
        """Hide this widget."""
        self.grid_remove()

    def default_write(self):
        """Write default stepper.var file if it does't exist."""
        if (os.path.exists(self.fixture_file)):
            return
        tbl = {}
        for i in range(len(_required_parameters)):
            tbl["%s" % (_required_parameters[i])] = 0.0
        tbl["5220"] = 1.0  # select G54 coordinate
        self.table_write(tbl)
        
    def button_state(self, s):
        self.ok_button.config(state=s)

    def table_reload(self):
        """Reload dialog from file."""
        self.table = self.table_read()
        self.update(self.gcode_sel_old)

if __name__ == "__main__":
    logging.basicConfig(stream=sys.stdout, level=logging.DEBUG, format='%(message)s')

    root = tkinter.Tk()
    fixture_file = "%s/.pymini/stepper.var" % (os.path.expanduser("~"))
    d = FixtureTable(root, fixture_file)

