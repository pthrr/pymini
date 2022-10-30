#!/usr/bin/python
# backplot.py - gcode ploting module. Used by pymini.py.
#
# (c) 2014-2015 Copyright Eckler Software
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

import logging, re
import math
from collections import OrderedDict
import pycircle

EPSILON_AE = 0.0000001  # epsilon with absolute error

class GcodeColor(object):
    G0 = 0
    G1 = 1
    G2 = 2
    G3 = 3
    G33 = 4
    G76 = 4
    G83 = 4
    G81 = 4

class BackPlot(object):

    def __init__(self, bp):
        self.bp = bp
        self.list = OrderedDict()
        self.color = ["lime green", "black", "red", "blue", "purple", "yellow3"]

        self.x = 0.0
        self.y = 0.0
        self.z = 0.0
        self.a = 0.0
        self.b = 0.0
        self.c = 0.0

        self.x_center = 0.0
        self.y_center = 0.0

        self.x_last = 0.0
        self.y_last = 0.0
        self.x_next = 0.0
        self.y_next = 0.0

        # Active canvas width/height.
        self.width = int(self.bp.cget("width"))
        self.height = int(self.bp.cget("height"))

        # Set axis directions.  +1 one plots like graph paper with positive up and to the right.
        # To reverse an axis change its value to -1
        self.xdir = 1
        self.ydir = -1
        self.zdir = -1

        # Set the default size of the plot and can be thought of as number of
        # pixels per inch of machine motion.  The default (40) will give about .55 screen
        # inch per inch moved.
        self.mdpi = 40

        self.scaler = 1

        # Bind canvas resize event to resize().
        self.bp.bind('<Configure>', self.resize)
        # Bind mouse button scrolling events.
        self.bp.bind("<ButtonPress-1>", self.scroll_start)
        self.bp.bind("<B1-Motion>", self.scroll_move)

        self.i_regex = re.compile(r"(?P<i>(i(-)?\d+(.\d+)?))")
        self.j_regex = re.compile(r"(?P<j>(j(-)?\d+(.\d+)?))")
        self.r_regex = re.compile(r"(?P<r>(r(-)?\d+(.\d+)?))")
        self.g_regex = re.compile(r"(?P<g>(g(-)?\d+(.\d+)?))")

        self.x_abs_last = EPSILON_AE
        self.y_abs_last = EPSILON_AE
        self.z_abs_last = EPSILON_AE

        self.gcode_color_last = GcodeColor.G0   # default canvas line color
        self.gcode_cb = None        # modal gcode call back

        # Default to 3d view.
        self.plot_3d()

    def resize(self, event):
        # Calculate the size change.
        dx = abs(event.width - self.width)
        dy = abs(event.height - self.height)

        # Set current canvas size.
        self.width = event.width
        self.height = event.height

        # Ignore small spurious size changes.
        if (dx > 10 or dy > 10):
            self.center_plot()

    def scroll_start(self, event):
        self.bp.scan_mark(event.x, event.y)

    def scroll_move(self, event):
        self.bp.scan_dragto(event.x, event.y, gain=1)

    def vector(self):
        # 3D vector conversion
        # X Y and Z point is converted into polar notation
        # then rotated about the A B and C axis.
        # Finally to be converted back into rectangular co-ords.

        # Rotate about A - X axis
        angle = self.a * 0.01745329
        if (self.y != 0 or self.z != 0):
            angle = math.atan2(self.y, self.z) + angle
        vector = math.hypot(self.y, self.z)
        self.z = vector * math.cos(angle)
        self.y = vector * math.sin(angle)

        # Rotate about B - Y axis
        angle = self.b * 0.01745329
        if (self.x != 0 or self.z != 0):
            angle = math.atan2(self.z, self.x) + angle
        vector = math.hypot(self.x, self.z)
        self.x = vector * math.cos(angle)
        self.z = vector * math.sin(angle)

        # Rotate about C - Z axis
        angle = self.c * 0.01745329
        if (self.x != 0 or self.y != 0):
            angle = math.atan2(self.y, self.x) + angle
        vector = math.hypot(self.x, self.y)
        self.x = vector * math.cos(angle)
        self.y = vector * math.sin(angle)

    def plot_xy(self):
        self.x_rotate = -90
        self.y_rotate = 0.0
        self.z_rotate = 0.0
        self.redraw()

    def plot_xz(self):
        self.x_rotate = 0.0
        self.y_rotate = 0.0
        self.z_rotate = 0.0
        self.redraw()

    def plot_yz(self):
        self.x_rotate = 0.0
        self.y_rotate = 0.0
        self.z_rotate = 90
        self.redraw()

    def plot_3d(self):
        self.x_rotate = -27
        self.y_rotate = 17
        self.z_rotate = 30
        self.redraw()

    def zoom_out(self):
        self.scaler *= 2.0
        self.redraw()

    def zoom_in(self):
        self.scaler *= 0.5
        self.redraw()

    def clear_plot(self, pos):
        self.list.clear()
        self.x = pos['x'] * self.mdpi * self.xdir / self.scaler
        self.y = pos['y'] * self.mdpi * self.ydir / self.scaler
        self.z = pos['z'] * self.mdpi * self.zdir / self.scaler
        self.vector()
        self.x_last = self.x
        self.y_last = self.z
        self.redraw()

    def redraw(self):
        self.bp.delete("all")
        self.center_plot()

        if (len(self.list) >= 2):
            # Loop through all the positions.
            i = 0
            for k, pos in self.list.items():
                if (i == 0):
                    # Set the first position.
                    self.x = pos['x'] * self.mdpi * self.xdir / self.scaler
                    self.y = pos['y'] * self.mdpi * self.ydir / self.scaler
                    self.z = pos['z'] * self.mdpi * self.zdir / self.scaler
                    self.a = self.x_rotate
                    self.b = self.y_rotate
                    self.c = self.z_rotate
                    self.vector()
                    self.x_last = self.x
                    self.y_last = self.z
                    i += 1
                    continue

                self.x = pos['x'] * self.mdpi * self.xdir / self.scaler
                self.y = pos['y'] * self.mdpi * self.ydir / self.scaler
                self.z = pos['z'] * self.mdpi * self.zdir / self.scaler
                self.vector()
                self.x_next = self.x
                self.y_next = self.z
                self.bp.create_line(self.x_last, self.y_last, self.x_next, self.y_next, fill=self.color[pos['gcode']])
                self.x_last = self.x
                self.y_last = self.z

        # Draw red arrow tick mark.
        self.tick = self.bp.create_line(self.x_last, self.y_last, self.x_last+5, self.y_last+5, fill="red", arrow="first", tags="tick_mark")

    def center_plot(self):
        # Calculate visual window and scroll window extents.
        vx1 = -self.width / 2
        vx2 = self.width / 2
        sx1 = -self.width * 1.9 / 2
        sx2 = self.width * 1.9 / 2
        vy1 = -self.height / 2
        vy2 = self.height / 2
        sy1 = -self.height * 1.9 / 2
        sy2 = self.height * 1.9 / 2

        # Center origin (0,0) in middle of scroll window.
        self.bp.config(scrollregion=(sx1, sy1, sx2, sy2))

        # Draw crosshatch.
        self.bp.create_line(sx1, 0, sx2, 0, fill="dark gray")
        self.bp.create_line(0, sy1, 0, sy2, fill="dark gray")

        # Move visual window to center.
        self.bp.xview_moveto((vx1 - sx1) / (sx2 - sx1))
        self.bp.yview_moveto((vy1 - sy1) / (sy2 - sy1))

    # circle_circle_intersection(), original author Tim Voght, 3/26/2005 (public domain).
    def circle_circle_intersection(self,x0, y0, x1, y1, r):
        """Given two points on a circle and a radius calculate center-1 and center-2."""
        r0 = r1 = r

        # dx and dy are the vertical and horizontal distances between the circle centers.
        dx = x1 - x0
        dy = y1 - y0

        # Determine the straight-line distance between the centers.
        d = math.sqrt((dy*dy) + (dx*dx))

        # Check for solvability.
        if (d > (r0 + r1)):
            if (abs(d - (r0 + r1)) > EPSILON_AE):
                raise Exception("No solution circles do not intersect.") 
        if (d < abs(r0 - r1)):
            raise Exception("No solution one circle is contained in the other.") 

        #
        # 'point 2' is the point where the line through the circle intersection points crosses 
        # the line between the circle centers.
        #

        # Determine the distance from point 0 to point 2.
        a = ((r0*r0) - (r1*r1) + (d*d)) / (2.0 * d)

        # Determine the coordinates of point 2.
        x2 = x0 + (dx * a/d)
        y2 = y0 + (dy * a/d)

        # Determine the distance from point 2 to either of the intersection points.
        h = math.sqrt(abs((r0*r0) - (a*a)))

        # Now determine the offsets of the intersection points from point 2.
        rx = -dy * (h/d)
        ry = dx * (h/d)

        # Determine the absolute intersection points.
        xi = x2 + rx
        xi_prime = x2 - rx
        yi = y2 + ry
        yi_prime = y2 - ry

        return xi, yi, xi_prime, yi_prime

    def get_arc_size_cc(self, circle):
        size = 0
        i = circle.start_index
        max = len(circle.list) - 1
        while (i != circle.end_index):
            size += 1
            i = (i + 1) % max      # increment
        return size

    def get_arc_size_c(self, circle):
        size = 0
        i = circle.start_index
        max = len(circle.list) - 1
        while (i != circle.end_index):
            size += 1
            i = (i + (max-1)) % max   # decrement
        return size

    def arc_cc(self, gcode, circle):
        """Generate an arc using line segments going counterclockwise."""
        self.a = self.x_rotate
        self.b = self.y_rotate
        self.c = self.z_rotate
        i = circle.start_index
        max = len(circle.list) - 1
        while (i != circle.end_index):
            p = circle.list[i]
            self.x = p['x'] * self.mdpi * self.xdir / self.scaler
            self.y = p['y'] * self.mdpi * self.ydir / self.scaler
            self.z = self.z_abs_last * self.mdpi * self.zdir / self.scaler
            self.vector()
            self.x_next = self.x
            self.y_next = self.z
            item = self.bp.create_line(self.x_last, self.y_last, self.x_next, self.y_next, fill=self.color[gcode])
            self.list[item] = {'x':p['x'], 'y':p['y'], 'z':self.z_abs_last, 'gcode':gcode}
            self.x_last = self.x
            self.y_last = self.z
            i = (i + 1) % max      # increment
        p = circle.list[i]
        self.x = p['x'] * self.mdpi * self.xdir / self.scaler
        self.y = p['y'] * self.mdpi * self.ydir / self.scaler
        self.z = self.z_abs_last * self.mdpi * self.zdir / self.scaler
        self.vector()
        self.x_next = self.x
        self.y_next = self.z
        item = self.bp.create_line(self.x_last, self.y_last, self.x_next, self.y_next, fill=self.color[gcode])
        self.list[item] = {'x':p['x'], 'y':p['y'], 'z':self.z_abs_last, 'gcode':gcode}
        self.x_last = self.x
        self.y_last = self.z

    def arc_c(self, gcode, circle):
        """Generate an arc using line segments going clockwise."""
        self.a = self.x_rotate
        self.b = self.y_rotate
        self.c = self.z_rotate
        i = circle.start_index
        max = len(circle.list) - 1
        while (i != circle.end_index):
            p = circle.list[i]
            self.x = p['x'] * self.mdpi * self.xdir / self.scaler
            self.y = p['y'] * self.mdpi * self.ydir / self.scaler
            self.z = self.z_abs_last * self.mdpi * self.zdir / self.scaler
            self.vector()
            self.x_next = self.x
            self.y_next = self.z
            item = self.bp.create_line(self.x_last, self.y_last, self.x_next, self.y_next, fill=self.color[gcode])
            self.list[item] = {'x':p['x'], 'y':p['y'], 'z':self.z_abs_last, 'gcode':gcode}
            self.x_last = self.x
            self.y_last = self.z
            i = (i + (max-1)) % max   # decrement
        p = circle.list[i]
        self.x = p['x'] * self.mdpi * self.xdir / self.scaler
        self.y = p['y'] * self.mdpi * self.ydir / self.scaler
        self.z = self.z_abs_last * self.mdpi * self.zdir / self.scaler
        self.vector()
        self.x_next = self.x
        self.y_next = self.z
        item = self.bp.create_line(self.x_last, self.y_last, self.x_next, self.y_next, fill=self.color[gcode])
        self.list[item] = {'x':p['x'], 'y':p['y'], 'z':self.z_abs_last, 'gcode':gcode}
        self.x_last = self.x
        self.y_last = self.z
        i = (i + (max-1)) % max   # decrement

    def circular_counterclockwise_radius(self, gcode, line, end_pos):
        # Start point.
        x1 = self.x_abs_last
        y1 = self.y_abs_last

        # End point.
        x2 = end_pos['x']
        y2 = end_pos['y']

        # Radius.
        r = self.r_regex.search(line) # R parameter
        r = r.group('r')
        r = float(r[1:])

        # Calculate cartesian points center_1 and center_2.
        xc1, yc1, xc2, yc2 = self.circle_circle_intersection(x1, y1, x2, y2, r)

        # Get circular points for each circle.
        c1 = pycircle.CirclePoints(xc1,yc1,x1,y1,x2,y2,r)
        c2 = pycircle.CirclePoints(xc2,yc2,x1,y1,x2,y2,r)

        # Calculate the size of each arc.
        c1_size = self.get_arc_size_cc(c1)
        c2_size = self.get_arc_size_cc(c2)

        # Determine which arc to use.
        if (r < 0):
            # Use largest arc.
            if (c1_size > c2_size):
                circle = c1
            else:
                circle = c2
        else:
            # Use smallest arc.
            if (c1_size < c2_size):
                circle = c1
            else:
                circle = c2

        # Display the circular arc.
        self.arc_cc(gcode, circle)

    def circular_clockwise_radius(self, gcode, line, end_pos):
        # Start point.
        x1 = self.x_abs_last
        y1 = self.y_abs_last

        # End point.
        x2 = end_pos['x']
        y2 = end_pos['y']

        # Radius.
        r = self.r_regex.search(line) # R parameter
        r = r.group('r')
        r = float(r[1:])

        # Calculate cartesian points center_1 and center_2.
        xc1, yc1, xc2, yc2 = self.circle_circle_intersection(x1, y1, x2, y2, r)

        # Get circular points for each circle.
        c1 = pycircle.CirclePoints(xc1,yc1,x1,y1,x2,y2,r)
        c2 = pycircle.CirclePoints(xc2,yc2,x1,y1,x2,y2,r)

        # Calculate the size of each arc.
        c1_size = self.get_arc_size_c(c1)
        c2_size = self.get_arc_size_c(c2)

        # Determine which arc to use.
        if (r < 0):
            # Use largest arc.
            if (c1_size > c2_size):
                circle = c1
            else:
                circle = c2
        else:
            # Use smallest arc.
            if (c1_size < c2_size):
                circle = c1
            else:
                circle = c2

        # Display the circular arc.
        self.arc_c(gcode, circle)

    def circular_counterclockwise_ij(self, gcode, line, end_pos):
        # Start point.
        x1 = self.x_abs_last
        y1 = self.y_abs_last

        # End point.
        x2 = end_pos['x']
        y2 = end_pos['y']

        # Radius.
        adj = self.i_regex.search(line) # I parameter
        adj = adj.group('i')
        adj = float(adj[1:])
        opp = self.j_regex.search(line) # J parameter
        opp = opp.group('j')
        opp = float(opp[1:])
        r = math.sqrt(adj*adj + opp*opp)

        # Center.
        x0 = x1 + adj
        y0 = y1 + opp

        # Get circular points between two points counterclockwise.
        c1 = pycircle.CirclePoints(x0,y0,x1,y1,x2,y2,r)

        # Display the circular arc.
        self.arc_cc(gcode, c1)

    def circular_clockwise_ij(self, gcode, line, end_pos):
        # Start point.
        x1 = self.x_abs_last
        y1 = self.y_abs_last

        # End point.
        x2 = end_pos['x']
        y2 = end_pos['y']

        # Radius.
        adj = self.i_regex.search(line) # I parameter
        adj = adj.group('i')
        adj = float(adj[1:])
        opp = self.j_regex.search(line) # J parameter
        opp = opp.group('j')
        opp = float(opp[1:])
        r = math.sqrt(adj*adj + opp*opp)

        # Center.
        x0 = x1 + adj
        y0 = y1 + opp

        # Get circular points between two points clockwise.
        c1 = pycircle.CirclePoints(x0,y0,x1,y1,x2,y2,r)

        # Display the circular arc.
        self.arc_c(gcode, c1)

    def circular_counterclockwise(self, gcode, line, end_pos):
        # Last tick position.
        xtick = self.x_last
        ytick = self.y_last

        # For now only display xy plane.
        if "i" in line and "j" in line:
            self.circular_counterclockwise_ij(gcode, line, end_pos)
        elif "r" in line and "x" in line and "y" in line:
            self.circular_counterclockwise_radius(gcode, line, end_pos)

        # Move the red arrow tick mark to the new position.
        self.bp.move(self.tick, self.x_next - xtick, self.y_next - ytick)

        # Save gcode for potential modal execution lines.
        self.gcode_color_last = gcode
        self.gcode_cb = self.circular_counterclockwise

    def circular_clockwise(self, gcode, line, end_pos):
        # Last tick position.
        xtick = self.x_last
        ytick = self.y_last

        # For now only display xy plane.
        if "i" in line and "j" in line:
            self.circular_clockwise_ij(gcode, line, end_pos)
        elif "r" in line and "x" in line and "y" in line:
            self.circular_clockwise_radius(gcode, line, end_pos)

        # Move the red arrow tick mark to the new position.
        self.bp.move(self.tick, self.x_next - xtick, self.y_next - ytick)

        # Save gcode for potential modal execution lines.
        self.gcode_color_last = gcode
        self.gcode_cb = self.circular_clockwise
            
    def straight_line(self, gcode, line, pos):
        self.x = pos['x'] * self.mdpi * self.xdir / self.scaler
        self.y = pos['y'] * self.mdpi * self.ydir / self.scaler
        self.z = pos['z'] * self.mdpi * self.zdir / self.scaler

        self.a = self.x_rotate
        self.b = self.y_rotate
        self.c = self.z_rotate

        self.vector()
        self.x_next = self.x
        self.y_next = self.z
        item = self.bp.create_line(self.x_last, self.y_last, self.x_next, self.y_next, fill=self.color[gcode])
        self.list[item] = {'x':pos['x'], 'y':pos['y'], 'z':pos['z'], 'gcode':gcode}

        # Move the red arrow tick mark to the new position.
        self.bp.move(self.tick, self.x_next - self.x_last, self.y_next - self.y_last)
        self.x_last = self.x
        self.y_last = self.z

        # Save gcode for potential modal execution lines.
        self.gcode_color_last = gcode
        self.gcode_cb = self.straight_line

    def dispatch_code(self, g_cmd, line, line_num, pos):
        if g_cmd == "g03":
            self.circular_counterclockwise(GcodeColor.G3, line, pos)
        elif g_cmd == "g3":
            self.circular_counterclockwise(GcodeColor.G3, line, pos)
        elif g_cmd == "g02":
            self.circular_clockwise(GcodeColor.G2, line, pos)
        elif g_cmd == "g2":
            self.circular_clockwise(GcodeColor.G2, line, pos)
        elif g_cmd == "g01":
            self.straight_line(GcodeColor.G1, line, pos)
        elif g_cmd == "g1":  # linear move
            self.straight_line(GcodeColor.G1, line, pos)
        elif g_cmd == "g0":  # rapid move
            self.straight_line(GcodeColor.G0, line, pos)
        elif g_cmd == "g33":  # spindle synchronized motion
            self.straight_line(GcodeColor.G33, line, pos)
        elif g_cmd == "g76":  # threading cycle
            self.straight_line(GcodeColor.G76, line, pos)
        elif g_cmd == "g83":  # pick drill cycle
            self.straight_line(GcodeColor.G83, line, pos)
        elif g_cmd == "g81":  # drill cycle
            self.straight_line(GcodeColor.G81, line, pos)

    def dispatch_line(self, g_cmd1, g_cmd2, line, line_num, pos):
        if (g_cmd1 == None):
            if (self.gcode_cb):
                self.gcode_cb(self.gcode_color_last, line, pos) # no gcode repeat last gcode with new position
            return

        if (g_cmd1 == "g53" and g_cmd2 == None):
            if (self.gcode_cb):
                self.gcode_cb(self.gcode_color_last, line, pos) # repeat last gcode with new position
            return

        # For now only display XY plane circular moves, ignore ZX and YZ plane.
        if (g_cmd1 == "g19" or g_cmd1 == "g18"):
            return

        self.dispatch_code(g_cmd1, line, line_num, pos)
        if (g_cmd2):
            self.dispatch_code(g_cmd2, line, line_num, pos)

    def update_plot(self, line, line_num, pos):
        # Check for duplicate position.
        if (abs(self.x_abs_last-pos['x']) < EPSILON_AE and abs(self.y_abs_last-pos['y']) < EPSILON_AE and abs(self.z_abs_last-pos['z']) < EPSILON_AE):
            return

        g_cmd1 = g_cmd2 = None

        # Parse gcode line and determine canvas line color.
        ln = line.lower()
        g = self.g_regex.search(ln) # find first G code
        if (g):
            g_cmd1 = g.group('g')
            g = self.g_regex.search(ln, g.end()) # find second G code
            if (g):
                g_cmd2 = g.group('g')
        self.dispatch_line(g_cmd1, g_cmd2, ln, line_num, pos)

        self.x_abs_last = pos['x']
        self.y_abs_last = pos['y']
        self.z_abs_last = pos['z']

################################################################################################################
if __name__ == "__main__":
    # Configure logging for the application.
    logging.basicConfig(filename='log.txt',
                        level=logging.DEBUG,
                        format='%(asctime)s:%(levelname)s:%(filename)s:%(lineno)s:%(message)s')

    b = BackPlot()
    b.vector()
