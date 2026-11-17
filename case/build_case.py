import FreeCAD
import Part
import Mesh
import math
from FreeCAD import Base

def v(x, y, z=0.0):
    return Base.Vector(float(x), float(y), float(z))

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

def export_stl(shape, filepath, lin_tol=0.05, ang_tol=0.2):
    mesh = Mesh.Mesh()
    mesh.addFacets(shape.tessellate(lin_tol))
    mesh.write(filepath)
    print(f"Exported: {filepath} ({mesh.CountFacets} facets)")

# Dimensions
WIDTH  = 58.0
HEIGHT = 102.0
DEPTH  = 13.0
WALL   = 2.0
RADIUS = 7.5

# Y coordinates (Center is Y=0, Top is Y=+51, Bottom is Y=-51)
# Screen window: Center Y = +33 (18mm from top)
SCREEN_CY = 33.0
SCREEN_W  = 24.0
SCREEN_H  = 13.0
OLED_PCB_W = 28.0
OLED_PCB_H = 28.0

# 4 Buttons: Center Y = +15 (36mm from top)
BTN_CY = 15.0
BTN_D  = 8.2
BTN_XS = [-18.0, -6.0, 6.0, 18.0]

# Wheel: Center Y = -18 (69mm from top)
WHEEL_CY = -18.0
WHEEL_RECESS_D = 43.0
WHEEL_RECESS_DEPTH = 2.2
CENTER_BTN_D = 14.0

print("Building Front Shell...")
# 1. Outer body
outer = make_rbox(WIDTH, HEIGHT, DEPTH, RADIUS, 0.0)

# 2. Inner cavity (open at bottom Z=0, front wall thickness = 2.4mm -> Z_top = 10.6)
inner = make_rbox(WIDTH - WALL*2, HEIGHT - WALL*2, DEPTH - 2.4, RADIUS - WALL, -0.1)
shell = outer.cut(inner)

# 3. Screen window (through front wall)
win = Part.makeBox(SCREEN_W, SCREEN_H, 4.0, v(-SCREEN_W/2, SCREEN_CY - SCREEN_H/2, DEPTH - 3.0))
shell = shell.cut(win)

# OLED PCB pocket inside (Z from 8.0 to 11.0)
oled_pocket = Part.makeBox(OLED_PCB_W, OLED_PCB_H, 2.0, v(-OLED_PCB_W/2, SCREEN_CY - OLED_PCB_H/2, DEPTH - 2.4 - 1.0))
shell = shell.cut(oled_pocket)

# 4. 4 Button holes
for bx in BTN_XS:
    hole = Part.makeCylinder(BTN_D / 2.0, 5.0, v(bx, BTN_CY, DEPTH - 4.0))
    shell = shell.cut(hole)

# 5. Wheel Recess
recess = Part.makeCylinder(WHEEL_RECESS_D / 2.0, WHEEL_RECESS_DEPTH + 0.1, v(0, WHEEL_CY, DEPTH - WHEEL_RECESS_DEPTH))
shell = shell.cut(recess)

# Center Button Hole (through)
center_hole = Part.makeCylinder(CENTER_BTN_D / 2.0, 5.0, v(0, WHEEL_CY, DEPTH - 4.0))
shell = shell.cut(center_hole)

# Encoder axle slot in the floor of the recess (so mouse encoder axle reaches wheel)
enc_axle_slot = Part.makeCylinder(4.0, 5.0, v(0, WHEEL_CY - 10.0, DEPTH - 4.0))
shell = shell.cut(enc_axle_slot)

# 6. USB-C cutout at bottom
usb = Part.makeBox(10.5, 6.0, 5.5, v(-10.5/2, -HEIGHT/2 - 1.0, 3.5))
shell = shell.cut(usb)

# 7. Add 2 internal screw bosses (for optional rear screws)
boss1 = Part.makeCylinder(2.5, DEPTH - 3.0, v(-WIDTH/2 + 3.5, 0, 0))
boss1 = boss1.cut(Part.makeCylinder(1.1, DEPTH - 3.0, v(-WIDTH/2 + 3.5, 0, 0)))
boss2 = Part.makeCylinder(2.5, DEPTH - 3.0, v(WIDTH/2 - 3.5, 0, 0))
boss2 = boss2.cut(Part.makeCylinder(1.1, DEPTH - 3.0, v(WIDTH/2 - 3.5, 0, 0)))
shell = shell.fuse(boss1).fuse(boss2)

print("Front Shell complete. Volume:", shell.Volume)
export_stl(shell, "d:/iPodify_ESP32_C3/case/iPodify_Front_Shell.stl")
