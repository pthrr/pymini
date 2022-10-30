#!/usr/bin/python
# pycircle.py - circular interpolation between the two point on a circle.
#
# (c) 2017 Copyright Eckler Software
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
# Notes:
# Circular polar method (trigonometric functions) was much smoother, with sparce points, 
# than circle midpoint algorithm. 
#

import math

DELTA_DEG = 3   # angle change in degrees
OCTANT_SIZE = 15   # 45 / DELTA_DEG

# Degree to list[] index map.
delta_map = [0] * (45 + 1)
delta_map[3] = 1
delta_map[6] = 2
delta_map[9] = 3
delta_map[12] = 4
delta_map[15] = 5
delta_map[18] = 6
delta_map[21] = 7
delta_map[24] = 8
delta_map[27] = 9
delta_map[30] = 10
delta_map[33] = 11
delta_map[36] = 12
delta_map[39] = 13
delta_map[42] = 14
delta_map[45] = 15

class DxfFile(object):
    NAME = "circle.dxf"

class Octant(object):
    o1 = 0    # 0 offset (degrees)
    o2 = OCTANT_SIZE
    o3 = OCTANT_SIZE*2
    o4 = OCTANT_SIZE*3
    o5 = OCTANT_SIZE*4
    o6 = OCTANT_SIZE*5
    o7 = OCTANT_SIZE*6
    o8 = OCTANT_SIZE*7

class CirclePoints(object):
    def __init__(self,x0,y0,x1,y1,x2,y2,r,dxf=None):
        self.x1 = x1  # start cartesian point
        self.y1 = y1
        self.x2 = x2  # end cartesian point
        self.y2 = y2
        self.dxf = dxf  # dxf file name for debug
        self.start_dif = 10000.0
        self.start_index = 0 # list[start_index] = start cartesian point
        self.end_dif = 10000.0
        self.end_index = 0   # list[end_index] = end cartesian point

        # Fill the list.
        self.circle_polar(x0,y0,r)

    def dxf_start(self):
        if (self.dxf):
            with open(self.dxf, 'w') as f:
                f.write("0\n")
                f.write("SECTION\n")
                f.write("2\n")
                f.write("ENTITIES\n")

    def dxf_end(self):
        if (self.dxf):
            with open(self.dxf, 'a') as f:
                f.write("0\n")
                f.write("ENDSEC\n")
                f.write("0\n")
                f.write("EOF\n")

    def dxf_point(self, x, y):
        if (self.dxf):
            with open(self.dxf, 'a') as f:
                f.write("0\n")
                f.write("POINT\n")
                f.write("8\n")
                f.write("0\n")      # layer number 
                f.write("10\n")
                f.write("%0.8f\n" % x)  # x 
                f.write("20\n")
                f.write("%0.8f\n" % y)      # y 
                f.write("30\n")
                f.write("0.0\n")      # z unused

    def dxf_circle(self, x, y, r):
        if (self.dxf):
            with open(self.dxf, 'a') as f:
                f.write("0\n")
                f.write("CIRCLE\n")
                f.write("8\n")
                f.write("0\n")     # layer number 
                f.write("10\n")
                f.write("%0.8f\n" % x)  # center x 
                f.write("20\n")
                f.write("%0.8f\n" % y)      # center y
                f.write("30\n")
                f.write("0.0\n")      # z unused
                f.write("40\n")
                f.write("%0.8f\n" % r)     # radius

    def isclose(self, a, b, rel_tol=1e-09, abs_tol=0.0):
        return abs(a-b) <= max(rel_tol * max(abs(a), abs(b)), abs_tol)

    def list_point(self, index, x, y):
        """Given the array index insert point in list. Note 360 = 0 degrees.""" 
        if (not (self.list[index]['x'] == 0 and self.list[index]['y'] == 0)):
            if (self.isclose(self.list[index]['x'], x) and self.isclose(self.list[index]['y'], y)):
                return # ok, overlap occur at 45, 90, 135, 180, 225, 270 and 315 degree
            else:
                raise Exception("overwrite x=%f, y=%f" % (x,y))
        self.list[index] = {'x':x, 'y':y}

        # Look for start point in list.
        dif = abs(self.x1 - x) + abs(self.y1 - y)
        if (dif < self.start_dif):
            self.start_dif = dif
            self.start_index = index # save index

        # Look for end point in list.
        dif = abs(self.x2 - x) + abs(self.y2 - y)
        if (dif < self.end_dif):
            self.end_dif = dif
            self.end_index = index # save index

    def circle_points(self, deg, x0, y0, x, y):
        """Insert point into all 8 octants keeping the list in sorted order (0-360 degrees, octant1-8)."""
        i = delta_map[deg]
        self.list_point(Octant.o1+i, x0 + x, y0 + y)  # octant 1
        self.list_point(Octant.o2+OCTANT_SIZE-i, x0 + y, y0 + x)  # octant 2
        self.list_point(Octant.o3+i, x0 - y, y0 + x)  # octant 3
        self.list_point(Octant.o4+OCTANT_SIZE-i, x0 - x, y0 + y)  # octant 4
        self.list_point(Octant.o5+i, x0 - x, y0 - y)  # octant 5
        self.list_point(Octant.o6+OCTANT_SIZE-i, x0 - y, y0 - x)  # octant 6
        self.list_point(Octant.o7+i, x0 + y, y0 - x)  # octant 7
        self.list_point(Octant.o8+OCTANT_SIZE-i, x0 + x, y0 - y)  # octant 8

    def circle_polar(self,x0,y0,radius):
        """Use circle polar methond algorithm to generate a circle of points given center and radius.
           By using 8-way symmetry, one only has to calculate one octant to find all points on the circle.  
           While building the list[] determine the start point and end point. This will be the circular
           interpolation between the two point on the circle."""

        theta_deg = 0  # angle start
        theta = math.radians(theta_deg)
        delta = math.radians(DELTA_DEG)

        # Create an empty list for all points on the circle (8 octants).
        self.list = [{'x':0, 'y':0}] * (OCTANT_SIZE * 8 + 1)

        # Fill the list in sorted order (0-360 degrees, octant1-8). 
        x = radius * math.cos(theta)
        y = radius * math.sin(theta)
        while (x > y or self.isclose(x, y)):
            self.circle_points(theta_deg, x0, y0, x, y)
            theta += delta
            theta_deg += DELTA_DEG
            x = radius * math.cos(theta)
            y = radius * math.sin(theta)

        # Refine start and end points.
        self.list[self.start_index]['x'] = self.x1
        self.list[self.start_index]['y'] = self.y1
        self.list[self.end_index]['x'] = self.x2
        self.list[self.end_index]['y'] = self.y2

        """
        self.dxf_start()
        i = self.start_index
        while (i <= self.end_index):
            p = self.list[i]
            self.dxf_point(p['x'], p['y'])
            i += 1
        self.dxf_end()

        self.dxf = "circle3.dxf"
        self.dxf_start()
        for p in self.list:
            self.dxf_point(p['x'], p['y'])
        self.dxf_end()

        print("start i=%d x=%f y=%f" % (self.start_index, self.list[self.start_index]['x'], self.list[self.start_index]['y']))
        print("end i=%d x=%f y=%f" % (self.end_index, self.list[self.end_index]['x'], self.list[self.end_index]['y']))
        """

################################################################################################################
if __name__ == "__main__":
    x1 = -1.135
    y1 = -0.020
    x2 = -2.092
    y2 = 1.046
    x0 = -2.2073
    y0 = -0.0199
    r = 1.0725
    """
    x1 = 1.5918   # 0 index
    y1 = 1.3795
    x2 = 1.4943
    y2 = 1.2575
    x0 = 1.4668
    y0 = 1.3795
    r = 0.125

    x1 = 2.5522   # wrap around
    y1 = -1.0504
    x2 = 2.825
    y2 = 0.7599
    x0 = 1.7793
    y0 = -0.0082
    r = 1.29751888233
    """
    cc = CirclePoints(x0,y0,x1,y1,x2,y2,r,DxfFile.NAME)
