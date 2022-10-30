#!/usr/bin/python
# tooltable.py - tool offset table widget. Used by pymini.py.
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
    import configparser
except ImportError:
    import Tkinter as tkinter
    import ConfigParser as configparser

class ToolTable(tkinter.Frame):
    """ToolTable widget"""
    def __init__(self, parent=None, tool_file="", lathe=False):
        tkinter.Frame.__init__(self, parent)
        self.tool_file = tool_file
        self.tool_sel = 1
        self.parent = parent
        self.result = None
        self.lathe = lathe

        self.table = self.table_read()

        grow = 0
        self.toolnum_label = tkinter.Label(self, text="toolnum")
        self.toolnum_label.grid(row=0, column=0, sticky='ew')
        self.toolnum_val = tkinter.StringVar()
        self.toolnum_entry = tkinter.Label(self, textvariable=self.toolnum_val, relief=tkinter.SUNKEN)
        self.toolnum_entry.grid(row=0, column=1, columnspan=3, sticky='ew')

        if (self.lathe):
            grow += 1
            self.txlength_label = tkinter.Label(self, text="x_length")
            self.txlength_label.grid(row=grow, column=0, sticky='ew')
            self.txlength_val = tkinter.StringVar()
            self.txlength_entry = tkinter.Entry(self, textvariable=self.txlength_val)
            self.txlength_entry.grid(row=grow, column=1, columnspan=3, sticky='ew')

        grow += 1
        self.tzlength_label = tkinter.Label(self, text="z_length")
        self.tzlength_label.grid(row=grow, column=0, sticky='ew')
        self.tzlength_val = tkinter.StringVar()
        self.tzlength_entry = tkinter.Entry(self, textvariable=self.tzlength_val)
        self.tzlength_entry.grid(row=grow, column=1, columnspan=3, sticky='ew')

        grow += 1
        if (self.lathe):
            self.tdiameter_label = tkinter.Label(self, text="radius")
        else:
            self.tdiameter_label = tkinter.Label(self, text="diameter")
        self.tdiameter_label.grid(row=grow, column=0, sticky='ew')
        self.tdiameter_val = tkinter.StringVar()
        self.tdiameter_entry = tkinter.Entry(self, textvariable=self.tdiameter_val)
        self.tdiameter_entry.grid(row=grow, column=1, columnspan=3, sticky='ew')

        if (self.lathe):
            grow += 1
            self.tfrontangle_label = tkinter.Label(self, text="front_angle")
            self.tfrontangle_label.grid(row=grow, column=0, sticky='ew')
            self.tfrontangle_val = tkinter.StringVar()
            self.tfrontangle_entry = tkinter.Entry(self, textvariable=self.tfrontangle_val)
            self.tfrontangle_entry.grid(row=grow, column=1, columnspan=3, sticky='ew')
            grow += 1
            self.tbackangle_label = tkinter.Label(self, text="back_angle")
            self.tbackangle_label.grid(row=grow, column=0, sticky='ew')
            self.tbackangle_val = tkinter.StringVar()
            self.tbackangle_entry = tkinter.Entry(self, textvariable=self.tbackangle_val)
            self.tbackangle_entry.grid(row=grow, column=1, columnspan=3, sticky='ew')
            grow += 1
            self.torientation_label = tkinter.Label(self, text="orientation")
            self.torientation_label.grid(row=grow, column=0, sticky='ew')
            self.torientation_val = tkinter.StringVar()
            self.torientation_entry = tkinter.Entry(self, textvariable=self.torientation_val)
            self.torientation_entry.grid(row=grow, column=1, columnspan=3, sticky='ew')
           
        grow += 1
        self.tcomment_label = tkinter.Label(self, text="comment")
        self.tcomment_label.grid(row=grow, column=0, sticky='ew')
        self.tcomment_val = tkinter.StringVar()
        self.tcomment_entry = tkinter.Entry(self, textvariable=self.tcomment_val)
        self.tcomment_entry.grid(row=grow, column=1, columnspan=3, sticky='ew')

        # Update table values.
        self.update()

        grow += 1
        self.back_button = tkinter.Button(self, text="Back", command=self.back)
        self.back_button.grid(row=grow, column=0)
        self.next_button = tkinter.Button(self, text="Next", command=self.next)
        self.next_button.grid(row=grow, column=1)
        self.ok_button = tkinter.Button(self, text="OK", command=self.ok)
        self.ok_button.grid(row=grow, column=2)
        self.cancel_button = tkinter.Button(self, text="Cancel", command=self.cancel)
        self.cancel_button.grid(row=grow, column=3)

    def save(self):
        """Write selected dialog entry into dictionary."""
        self.table[self.tool_sel]['zlength'] = float(self.tzlength_val.get())
        self.table[self.tool_sel]['diameter'] = float(self.tdiameter_val.get())
        self.table[self.tool_sel]['comment'] = self.tcomment_val.get()
        if (self.lathe):
            self.table[self.tool_sel]['xlength'] = float(self.txlength_val.get())
            self.table[self.tool_sel]['frontangle'] = float(self.tfrontangle_val.get())
            self.table[self.tool_sel]['backangle'] = float(self.tbackangle_val.get())
            orientation = int(self.torientation_val.get())
            if (orientation < 0 or orientation > 9):
                raise ValueError("invalid orientation value must be 0-9: '%d'" % (orientation))
            self.table[self.tool_sel]['orientation'] = orientation
        
    def update(self):
        """Write dictionary entry into dialog."""
        self.toolnum_val.set("%d" % (self.tool_sel))
        self.tzlength_val.set("%g" % (self.table[self.tool_sel]['zlength']))
        self.tdiameter_val.set("%g" % (self.table[self.tool_sel]['diameter']))
        self.tcomment_val.set("%s" % (self.table[self.tool_sel]['comment']))
        if (self.lathe):
            self.txlength_val.set("%g" % (self.table[self.tool_sel]['xlength']))
            self.tfrontangle_val.set("%g" % (self.table[self.tool_sel]['frontangle']))
            self.tbackangle_val.set("%g" % (self.table[self.tool_sel]['backangle']))
            self.torientation_val.set("%d" % (self.table[self.tool_sel]['orientation']))
                        
    def back(self):
        try:        
            self.save()
        except Exception as err:
            logging.error("Input error: %s" % (err))
            return  # leave dialog here, no back
        if (self.tool_sel > 1):
            self.tool_sel -= 1
        self.update()

    def next(self):
        try:        
            self.save()
        except Exception as err:
            logging.error("Input error: %s" % (err))
            return  # leave dialog here, no next
        if (self.tool_sel < len(self.table)):
            self.tool_sel += 1
        self.update()

    def ok(self, event=None):
        try:        
            self.save()
        except Exception as err:
            logging.error("Unable save tool offsets: %s" % (err))
            return  # leave dialog open

        self.result = self.table

        # Close old stepper.tbl file.
        self.parent.dog.close()
        try:
            self.table_write(self.table)
            logging.info("Saved tool offsets: %s" % (self.tool_file))            
            # Open new stepper.tbl file.
            self.parent.dog.open(self.parent.homedir, self.parent.inifile)           
        except Exception as err:
            logging.error("Unable save tool offsets: %s" % (err))
            return # leave dialog open
        self.cancel()

    def old_table_read(self):
        '''If v1 tool table file exists, convert it to v2. V1 tool table was mill only, no lathe support.'''
        tbl = {}
        with open(self.tool_file, 'r') as f:
            line = f.readline()
            while (line != ''):
                if (line[0] == '#'):
                    line = f.readline()
                    continue
                ln = line.split(None, 4)
                if (len(ln) != 5):
                    line = f.readline()
                    continue
                tool = {}
                tool['zlength'] = float(ln[2])
                tool['diameter'] = float(ln[3])
                tool['comment'] = ln[4].strip()
                tool['xlength'] = 0.0
                tool['frontangle'] = 0.0
                tool['backangle'] = 0.0
                tool['orientation'] = 0
                tbl[int(ln[0])] = tool
                line = f.readline()
        
        if (len(tbl) > 0):
            # Save old tool table (ie: stepper-v1.tbl) for use with pymini 1.xx.
            backup = "%s-v1%s" % (os.path.splitext(self.tool_file)[0], os.path.splitext(self.tool_file)[1])
            if (os.path.exists(backup)):
                os.remove(backup)
            os.rename(self.tool_file, backup)
            # Create new table from old tool table.
            self.table_write(tbl)

        return tbl

    def table_read(self):
        tbl = {}
        self.default_write()
        if (not os.path.exists(self.tool_file)):
            logging.error("Unable to load tool table file: %s" % (self.tool_file))
            return tbl

        # Try loading the new tool offset file.
        self.cfg = configparser.ConfigParser()
        try:
            self.cfg.read(self.tool_file)
        except:
            try:
                # Try reading old style tool table (pymini 1.xx).           
                return self.old_table_read()
            except:
                return tbl

        # Assume tool numbers are sequential 1-n.
        for i in range(1, len(self.cfg.sections())+1):
            tool = {}
            section = "T%d" % (i)
            tool['xlength'] = self.cfg.getfloat(section, "X_OFFSET")
            tool['zlength'] = self.cfg.getfloat(section, "Z_OFFSET")
            if (self.lathe):
                tool['diameter'] = self.cfg.getfloat(section, "DIAMETER") / 2.0  # convert diameter to radius
            else:
                tool['diameter'] = self.cfg.getfloat(section, "DIAMETER")              
            tool['frontangle'] = self.cfg.getfloat(section, "FRONT_ANGLE")
            tool['backangle'] = self.cfg.getfloat(section, "BACK_ANGLE")
            tool['orientation'] = self.cfg.getint(section, "ORIENTATION")
            tool['comment'] = self.cfg.get(section, "COMMENT")
            tbl[i] = tool

        return tbl

    def table_write(self, tbl):
        # Backup the old table.
        if (os.path.exists(self.tool_file)):
            backup = "%s.bak" % (self.tool_file)
            if (os.path.exists(backup)):
                os.remove(backup)
            os.rename(self.tool_file, backup)

        with open(self.tool_file, 'w') as f:
            f.write("# Tool table offset file for rtstepperemc software.")
            f.write("\n")
            for i in range(1, len(tbl)+1):
                tool = tbl.get(i)
                f.write("[T%d]\n" % (i))
                f.write("POCKET = %d\n" % (i))
                f.write("X_OFFSET = %g\n" % (tool['xlength']))
                f.write("Z_OFFSET = %g\n" % (tool['zlength']))
                if (self.lathe):
                    f.write("DIAMETER = %g\n" % (tool['diameter'] * 2.0))  # convert radius back to diameter
                else:
                    f.write("DIAMETER = %g\n" % (tool['diameter']))                    
                f.write("FRONT_ANGLE = %g\n" % (tool['frontangle']))
                f.write("BACK_ANGLE = %g\n" % (tool['backangle']))
                f.write("ORIENTATION = %d\n" % (tool['orientation']))
                f.write("COMMENT = %s\n" % (tool['comment']))
                f.write("\n")

    def cancel(self, event=None):
        """Hide this widget."""
        self.grid_remove()

    def default_write(self):
        """Write default stepper.tbl file if it does't exist."""
        if (os.path.exists(self.tool_file)):
            return
        # Create a default tool table with zero offsets.
        tbl = {}
        for i in range(1, 11):
            tool = {}
            tool['xlength'] = 0.0
            tool['zlength'] = 0.0
            tool['diameter'] = 0.0
            tool['frontangle'] = 0.0
            tool['backangle'] = 0.0
            tool['orientation'] = 0
            tool['comment'] = "empty"
            tbl[i] = tool
        self.table_write(tbl)
        
    def button_state(self, s):
        self.ok_button.config(state=s)

    def table_reload(self):
        """Reload dialog from file."""
        self.table = self.table_read()
        self.update()

if __name__ == "__main__":
    logging.basicConfig(stream=sys.stdout, level=logging.DEBUG, format='%(message)s')

    root = tkinter.Tk()
    tool_file = "%s/.pymini/stepper.tbl" % (os.path.expanduser("~"))
    d = ToolTable(root, tool_file)

