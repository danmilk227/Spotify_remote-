import FreeCAD
import Part
import Mesh
import math
from FreeCAD import Base

def v(x, y, z=0.0):
    return Base.Vector(x, y, z)

def make_rbox(w, h, d, r, z_base=0.0):
    """Rounded box centered at XY=(0,0), Z from z_base to z_base+d."""
    SQ45 = r * math.sqrt(2.0) / 2.0
    hw, hh = w / 2.0, h / 2.0
    cx, cy = hw - r, hh - r

    def seg(x0, y0, x1, y1):
        return Part.LineSegment(v(x0, y0, z_base), v(x1, y1, z_base)).toShape()

    def arc3(x0, y0, xm, ym, x1, y1):
        return Part.Arc(v(x0, y0, z_base), v(xm, ym, z_base), v(x1, y1, z_base)).toShape()

    edges = [
        seg(   cx, -hh, -cx, -hh),
        arc3( -cx, -hh, -cx - SQ45, -cy - SQ45, -hw, -cy),
        seg(  -hw,  -cy, -hw,  cy),
        arc3( -hw,   cy, -cx - SQ45,  cy + SQ45, -cx,  hh),
        seg(  -cx,   hh,  cx,  hh),
        arc3(  cx,   hh,  cx + SQ45,  cy + SQ45,  hw,  cy),
        seg(   hw,   cy,  hw, -cy),
        arc3(  hw,  -cy,  cx + SQ45, -cy - SQ45,  cx, -hh),
    ]
    wire = Part.Wire(edges)
    face = Part.Face(wire)
    return face.extrude(v(0, 0, d))

print("Testing geometry generation...")
box = make_rbox(60, 104, 13, 8)
print("Volume:", box.Volume)
