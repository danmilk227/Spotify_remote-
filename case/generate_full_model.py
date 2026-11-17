# -*- coding: utf-8 -*-
"""
=============================================================================
 iPodify ESP32-C3 — FreeCAD Full Model & STL Generator (v2.0)
=============================================================================
 Создает:
   1. iPodify_Front_Shell.stl  — Лицевой корпус (чистая морда, точные вырезы)
   2. iPodify_Back_Cover.stl   — Задняя крышка (защелка/бортик + направляющие)
   3. iPodify_Wheel_Disc.stl   — Отдельный диск колеса управления (iPod Click-Wheel)
   4. iPodify_Center_Button.stl— Центральная кнопка с юбкой от выпадения
   5. iPodify_Button_Caps.stl  — 4 кнопки-толкателя для верхнего ряда
   6. iPodify_Complete.FCStd   — Полный проект FreeCAD со всеми деталями
=============================================================================
"""

import FreeCAD
import Part
import Mesh
import math
from FreeCAD import Base

def v(x, y, z=0.0):
    return Base.Vector(float(x), float(y), float(z))

def make_rbox(w, h, d, r, z_base=0.0):
    """Скруглённый параллелепипед, центрирован по XY, Z от z_base до z_base+d."""
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

def export_stl(shape, filepath, lin_tol=0.04, ang_tol=0.15):
    mesh = Mesh.Mesh()
    mesh.addFacets(shape.tessellate(lin_tol))
    mesh.write(filepath)
    print("Exported STL: %s (%d facets)" % (filepath, mesh.CountFacets))

# ══════════════════════════════════════════════════════════════
# ПАРАМЕТРЫ КОРПУСА
# ══════════════════════════════════════════════════════════════
CW   = 58.0   # ширина корпуса
CH   = 102.0  # высота корпуса
CD   = 12.5   # глубина передней части
WALL = 2.0    # толщина боковых стенок
CR   = 7.5    # скругление углов

# Экран OLED 0.96"
SCREEN_CY  = 33.0   # Y центра окна (18 мм от верхнего края)
SCREEN_W   = 24.0
SCREEN_H   = 13.0
OLED_PCB_W = 28.0
OLED_PCB_H = 28.0

# 4 кнопки в ряд
BTN_CY = 15.0       # Y центра ряда (36 мм от верхнего края)
BTN_D  = 8.2        # Ø отверстий под толкатели
BTN_XS = [-18.0, -6.0, 6.0, 18.0]

# Диск колеса и центральная кнопка
WHEEL_CY     = -18.0 # Y центра диска (69 мм от верха)
WHEEL_WELL_D = 43.0  # Ø ниши под колесо
WHEEL_WELL_H = 2.2   # глубина ниши
CENTER_BTN_D = 14.0  # Ø отверстия под центр. кнопку

# ══════════════════════════════════════════════════════════════
# 1. ПЕРЕДНИЙ КОРПУС (Front Shell)
# ══════════════════════════════════════════════════════════════
print("--- [1/5] Building Front Shell ---")
outer = make_rbox(CW, CH, CD, CR, 0.0)

# Внутренняя полость (открыта сзади Z=0, толщина передней стенки 2.2 мм)
inner = make_rbox(CW - WALL*2, CH - WALL*2, CD - 2.2, CR - WALL, -0.1)
front_shell = outer.cut(inner)

# Окно экрана сквозное
win = Part.makeBox(SCREEN_W, SCREEN_H, 4.0, v(-SCREEN_W/2, SCREEN_CY - SCREEN_H/2, CD - 3.0))
front_shell = front_shell.cut(win)

# Внутренний карман для фиксации платы OLED (чтобы не елозила)
oled_pocket = Part.makeBox(OLED_PCB_W, OLED_PCB_H, 1.4, v(-OLED_PCB_W/2, SCREEN_CY - OLED_PCB_H/2, CD - 2.2 - 1.0))
front_shell = front_shell.cut(oled_pocket)

# 4 отверстия под кнопки
for bx in BTN_XS:
    hole = Part.makeCylinder(BTN_D / 2.0, 5.0, v(bx, BTN_CY, CD - 4.0))
    front_shell = front_shell.cut(hole)

# Ниша под отдельный диск колеса
recess = Part.makeCylinder(WHEEL_WELL_D / 2.0, WHEEL_WELL_H + 0.1, v(0, WHEEL_CY, CD - WHEEL_WELL_H))
front_shell = front_shell.cut(recess)

# Сквозное отверстие под кнопку CENTER в центре ниши
center_hole = Part.makeCylinder(CENTER_BTN_D / 2.0, 5.0, v(0, WHEEL_CY, CD - 4.0))
front_shell = front_shell.cut(center_hole)

# Окно для связи колеса с энкодером (ось/зацепление)
enc_slot = Part.makeCylinder(3.5, 5.0, v(0, WHEEL_CY - 10.0, CD - 4.0))
front_shell = front_shell.cut(enc_slot)

# Вырез под Type-C в нижнем торце (аккуратный 10.5 x 5.5 мм)
usb = Part.makeBox(10.5, 6.0, 5.5, v(-10.5/2, -CH/2 - 1.0, 3.2))
front_shell = front_shell.cut(usb)

# 4 угловых гнезда под направляющие штифты задней крышки
for xs in [-1, 1]:
    for ys in [-1, 1]:
        px = xs * (CW/2 - 4.5)
        py = ys * (CH/2 - 4.5)
        lug_hole = Part.makeCylinder(1.5, 4.0, v(px, py, 0))
        lug_boss = Part.makeCylinder(3.5, 3.5, v(px, py, 0))
        lug_boss = lug_boss.cut(lug_hole)
        front_shell = front_shell.fuse(lug_boss)

# 2 внутренние боковые бобышки под опциональные винты M2 со спины
boss_l = Part.makeCylinder(2.5, CD - 3.5, v(-CW/2 + 3.2, 0, 0))
boss_l = boss_l.cut(Part.makeCylinder(1.0, CD - 3.0, v(-CW/2 + 3.2, 0, 0)))
boss_r = Part.makeCylinder(2.5, CD - 3.5, v(CW/2 - 3.2, 0, 0))
boss_r = boss_r.cut(Part.makeCylinder(1.0, CD - 3.0, v(CW/2 - 3.2, 0, 0)))
front_shell = front_shell.fuse(boss_l).fuse(boss_r)

export_stl(front_shell, "d:/iPodify_ESP32_C3/case/iPodify_Front_Shell.stl")


# ══════════════════════════════════════════════════════════════
# 2. ЗАДНЯЯ КРЫШКА (Back Cover)
# ══════════════════════════════════════════════════════════════
print("--- [2/5] Building Back Cover ---")
BACK_THICK = 2.0
LIP_H      = 2.8
TOL        = 0.15 # зазор защелки 0.15 мм для плотной посадки

back_base = make_rbox(CW, CH, BACK_THICK, CR, 0.0)

# Защелкивающийся внутренний бортик по периметру
lip_outer = make_rbox(CW - WALL*2 - TOL*2, CH - WALL*2 - TOL*2, LIP_H, CR - WALL, BACK_THICK)
lip_inner = make_rbox(CW - WALL*2 - TOL*2 - 2.4, CH - WALL*2 - TOL*2 - 2.4, LIP_H + 0.1, CR - WALL, BACK_THICK - 0.05)
lip = lip_outer.cut(lip_inner)
back_cover = back_base.fuse(lip)

# 4 направляющих штифта в углах
for xs in [-1, 1]:
    for ys in [-1, 1]:
        px = xs * (CW/2 - 4.5)
        py = ys * (CH/2 - 4.5)
        pin = Part.makeCylinder(1.3, LIP_H + 1.5, v(px, py, BACK_THICK))
        back_cover = back_cover.fuse(pin)

# Паз под ноготь внизу для легкого открывания без инструмента
pry_slot = Part.makeBox(12.0, 3.0, 1.2, v(-6.0, -CH/2 - 0.5, 0.0))
back_cover = back_cover.cut(pry_slot)

# 2 отверстия с зенковкой под опциональные винты M2 (если захочешь прикрутить)
for sx in [-CW/2 + 3.2, CW/2 - 3.2]:
    hole = Part.makeCylinder(1.2, BACK_THICK + 1.0, v(sx, 0, -0.5))
    csink = Part.makeCone(2.5, 1.2, 1.2, v(sx, 0, 0))
    back_cover = back_cover.cut(hole).cut(csink)

export_stl(back_cover, "d:/iPodify_ESP32_C3/case/iPodify_Back_Cover.stl")


# ══════════════════════════════════════════════════════════════
# 3. ОТДЕЛЬНЫЙ ДИСК КОЛЕСА (Wheel Disc)
# ══════════════════════════════════════════════════════════════
print("--- [3/5] Building Wheel Disc ---")
WHEEL_OD = 42.4  # зазор 0.3 мм с нишей 43.0 мм
WHEEL_ID = 14.4  # зазор под центральную кнопку
WHEEL_TH = 2.4   # толщина диска

wheel = Part.makeCylinder(WHEEL_OD / 2.0, WHEEL_TH, v(0, 0, 0))
wheel = wheel.cut(Part.makeCylinder(WHEEL_ID / 2.0, WHEEL_TH + 0.2, v(0, 0, -0.1)))

# Тактильная круговая канавка на диске (iPod Touch-Wheel стиль)
groove = Part.makeCylinder(17.0, 0.6, v(0, 0, WHEEL_TH - 0.5))
groove = groove.cut(Part.makeCylinder(15.5, 0.7, v(0, 0, WHEEL_TH - 0.55)))
wheel = wheel.cut(groove)

# Тактильная выемка под палец (эргономичная ямочка для вращения)
dimple = Part.makeSphere(3.5, v(0, 14.0, WHEEL_TH + 2.5))
wheel = wheel.cut(dimple)

# Кольцевой выступ снизу для минимального трения о корпус
glide_ring = Part.makeCylinder(19.0, 0.5, v(0, 0, -0.5))
glide_ring = glide_ring.cut(Part.makeCylinder(17.5, 0.6, v(0, 0, -0.55)))
wheel = wheel.fuse(glide_ring)

export_stl(wheel, "d:/iPodify_ESP32_C3/case/iPodify_Wheel.stl")


# ══════════════════════════════════════════════════════════════
# 4. ЦЕНТРАЛЬНАЯ КНОПКА (Center Button Cap)
# ══════════════════════════════════════════════════════════════
print("--- [4/5] Building Center Button ---")
# Шляпка 13.6 мм, юбка 15.5 мм (не выпадет наружу), ножка под тактовую кнопку 6x6
c_cap = Part.makeCylinder(13.6 / 2.0, 2.5, v(0, 0, 0.8))
c_brim = Part.makeCylinder(15.5 / 2.0, 0.8, v(0, 0, 0))
c_stem = Part.makeCylinder(3.2 / 2.0, 2.5, v(0, 0, -2.5))
center_btn = c_cap.fuse(c_brim).fuse(c_stem)

export_stl(center_btn, "d:/iPodify_ESP32_C3/case/iPodify_Center_Button.stl")


# ══════════════════════════════════════════════════════════════
# 5. КОМПЛЕКТ КНОПОК-ТОЛКАТЕЛЕЙ (4 Button Caps)
# ══════════════════════════════════════════════════════════════
print("--- [5/5] Building Button Caps ---")
def make_single_button():
    b_cap = Part.makeCylinder(7.8 / 2.0, 2.5, v(0, 0, 0.8))
    b_brim = Part.makeCylinder(9.8 / 2.0, 0.8, v(0, 0, 0))
    b_stem = Part.makeCylinder(2.6 / 2.0, 2.5, v(0, 0, -2.5))
    return b_cap.fuse(b_brim).fuse(b_stem)

btn_set = Part.makeCompound([
    make_single_button().translated(v(-18.0, 0, 0)),
    make_single_button().translated(v(-6.0,  0, 0)),
    make_single_button().translated(v( 6.0,  0, 0)),
    make_single_button().translated(v( 18.0, 0, 0))
])

export_stl(btn_set, "d:/iPodify_ESP32_C3/case/iPodify_4_Buttons.stl")


# ══════════════════════════════════════════════════════════════
# СОХРАНЕНИЕ ПРОЕКТА FreeCAD (.FCStd)
# ══════════════════════════════════════════════════════════════
print("Creating FreeCAD document project...")
doc = FreeCAD.newDocument("iPodify_V2")

o1 = doc.addObject("Part::Feature", "FrontShell")
o1.Shape = front_shell

o2 = doc.addObject("Part::Feature", "BackCover")
o2.Shape = back_cover

o3 = doc.addObject("Part::Feature", "WheelDisc")
o3.Shape = wheel.translated(v(0, WHEEL_CY, CD - WHEEL_WELL_H + 0.3))

o4 = doc.addObject("Part::Feature", "CenterButton")
o4.Shape = center_btn.translated(v(0, WHEEL_CY, CD - WHEEL_WELL_H + 0.5))

o5 = doc.addObject("Part::Feature", "TopButtons")
o5.Shape = btn_set.translated(v(0, BTN_CY, CD - 1.5))

doc.recompute()
doc.saveAs("d:/iPodify_ESP32_C3/case/iPodify_Complete.FCStd")
print("Saved FreeCAD Project: d:/iPodify_ESP32_C3/case/iPodify_Complete.FCStd")
print("ALL DONE SUCCESSFULLY!")
