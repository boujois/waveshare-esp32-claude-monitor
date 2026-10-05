"""Desk stand for the Waveshare ESP32-S3-LCD-1.28-B (CNC metal case).

Design 9, "automotive": a domed base with a flat front, a ring-shaped cup
that the case sits in, and a hood behind the ring carrying a straight cable
tunnel along the screen axis. designs.py builds the other designs around
the same ring position and angle.

Generates designs/9-automotive.stl. Requires: pip install manifold3d trimesh numpy

Coordinates
  World: X = left/right, Y = away from the viewer (front face of the base
  at Y = 0), Z = up, desk at Z = 0.
  Device-local (a, b, c), origin at the centre of the case's back face:
    a = sideways, b = up the screen, c = out of the screen.
"""

import math

import numpy as np
import trimesh
from manifold3d import CrossSection, JoinType, Manifold, OpType

# ---- parameters (mm) -------------------------------------------------------
SCREEN_ANGLE = 75.0     # screen angle from the desk
CENTRE_Y = 5.7          # case back-face centre, behind the base front face
CENTRE_Z = 53.7         # case back-face centre, above the desk

# ring cup
PLATE_T = 1.0           # ring plate thickness (behind the case)
RING_OUTER_R = 25.1
FIT_TOLERANCE = 0.1     # extra on the case hole's diameter; raise if the case is tight
LIP_INNER_R = 24.0 + FIT_TOLERANCE / 2   # case body is 46 mm dia
LIP_DEPTH = 7.0         # how far the lip wraps the case side
HOLE_R = 12.4           # hole in the ring plate (also scoops the hood)
PLATE_BOTTOM = -23.85   # ring plate cut-off (b)
LIP_BOTTOM = -21.85     # lip cut-off (b), leaves the USB facet clear

# cable tunnel along the screen axis
TUNNEL_HALF_W = 8.0     # 16 mm wide
TUNNEL_TOP = -21.85     # b
TUNNEL_BOTTOM = -32.85  # b (11 mm tall)
TUNNEL_R = 1.0

# hood behind the ring
HOOD_HALF_W = 16.0
HOOD_TOP = -9.35        # b
HOOD_LENGTH = 27.0      # back from the case's back face
HOOD_R = 3.0            # rounding of hood top edges
HOOD_BACK_R = 5.0       # rounding of hood back corners

# domed base: an ellipsoid clipped to a flat front, flat sides and the desk
BASE_W = 70.0
BASE_D = 50.0
DOME_CENTRE = (0.0, 5.5, 3.1)
DOME_RADII = (47.8, 44.2, 35.4)
FRONT_CORNER_R = 22.5

# material left under the tunnel floor where it exits the back of the dome.
# The whole stand is lowered (and the dome trimmed at the desk) to achieve it;
# set to None to keep the heights above.
REAR_FLOOR = 1.0

# magnets glued into the underside: 8 x 2 mm neodymium buttons
MAGNETS = True
MAGNET_D, MAGNET_H = 8.0, 2.0
MAGNET_CLEAR = 0.4      # on diameter, for a smooth push fit with room for glue
MAGNET_EXTRA = 0.2      # extra depth so the magnet sits just below the surface
MAGNET_WALL = 3.0       # minimum plastic around each pocket
MAGNET_TOP = 1.0        # minimum plastic above each pocket

SEGMENTS = 128
BIG = 300.0
# ----------------------------------------------------------------------------

t = math.radians(90.0 - SCREEN_ANGLE)           # lean back from vertical
s, co = math.sin(t), math.cos(t)
V = np.array([0.0, s, co])                      # up the screen
N = np.array([0.0, -co, s])                     # out of the screen


def tunnel_exit_z():
    """Height of the tunnel floor centreline where it leaves the dome."""
    p0 = np.array([0.0, CENTRE_Y, CENTRE_Z]) + TUNNEL_BOTTOM * V
    q, d, r = p0 - np.array(DOME_CENTRE), -N, np.array(DOME_RADII)
    qa, da = q / r, d / r                       # solve |q + k d|_ellipsoid = 1
    A, B, C = da @ da, 2 * qa @ da, qa @ qa - 1
    k = (-B + math.sqrt(B * B - 4 * A * C)) / (2 * A)
    return float((p0 + k * d)[2])


if REAR_FLOOR is not None:
    DROP = tunnel_exit_z() - REAR_FLOOR
    CENTRE_Z -= DROP
    DOME_CENTRE = (DOME_CENTRE[0], DOME_CENTRE[1], DOME_CENTRE[2] - DROP)

# local (a, b, c) -> world; columns are X, V, N (a proper rotation)
LOCAL = [[1, 0, 0, 0], [0, s, -co, CENTRE_Y], [0, co, s, CENTRE_Z]]


def to_world(m):
    return m.transform(LOCAL)


def lbox(a0, a1, b0, b1, c0, c1):
    """Axis-aligned box in device-local coordinates, placed in the world."""
    return to_world(Manifold.cube([a1 - a0, b1 - b0, c1 - c0]).translate([a0, b0, c0]))


def lcyl(r, c0, c1):
    return to_world(Manifold.cylinder(c1 - c0, r, r, SEGMENTS).translate([0, 0, c0]))


def rounded_rect(x0, x1, y0, y1, r):
    return CrossSection.square([x1 - x0 - 2 * r, y1 - y0 - 2 * r]).translate(
        [x0 + r, y0 + r]).offset(r, circular_segments=32)


def world_halfspace(axis, lo):
    """Everything with world coordinate `axis` >= lo."""
    off = [-BIG / 2] * 3
    off[axis] = lo
    return Manifold.cube([BIG] * 3).translate(off)


# --- domed base ---
dome = Manifold.sphere(1.0, SEGMENTS).scale(list(DOME_RADII)).translate(list(DOME_CENTRE))
footprint = rounded_rect(-BASE_W / 2, BASE_W / 2, 0, BASE_D + FRONT_CORNER_R,
                         FRONT_CORNER_R)
base = (dome
        ^ Manifold.extrude(footprint, BIG)
        ^ Manifold.cube([BASE_W, BASE_D, BIG]).translate([-BASE_W / 2, 0, 0]))

# --- hood: rounded in both the a-b and b-c directions ---
hood_ab = rounded_rect(-HOOD_HALF_W, HOOD_HALF_W, -BIG, HOOD_TOP, HOOD_R)
hood_bc = rounded_rect(-BIG, HOOD_TOP, -HOOD_LENGTH, BIG, HOOD_R)
hood_ac = rounded_rect(-HOOD_HALF_W, HOOD_HALF_W, -BIG, HOOD_LENGTH, HOOD_BACK_R)  # (a, -c)
hood = (Manifold.extrude(hood_ab, HOOD_LENGTH).translate([0, 0, -HOOD_LENGTH])
        ^ Manifold.extrude(hood_bc, BIG).translate([0, 0, -BIG / 2])
        .transform([[0, 0, 1, 0], [1, 0, 0, 0], [0, 1, 0, 0]])
        ^ Manifold.extrude(hood_ac, BIG).translate([0, 0, -BIG / 2])
        .transform([[1, 0, 0, 0], [0, 0, 1, 0], [0, -1, 0, 0]]))
hood = to_world(hood) ^ world_halfspace(1, 0.0) ^ world_halfspace(2, 0.0)

# --- ring cup ---
ring = ((lcyl(RING_OUTER_R, -PLATE_T, LIP_DEPTH)
         ^ lbox(-BIG, BIG, PLATE_BOTTOM, BIG, -BIG, BIG))
        - lcyl(LIP_INNER_R, 0, BIG)
        - lbox(-BIG, BIG, -BIG, LIP_BOTTOM, 0, BIG))

# --- cuts ---
hole = lcyl(HOLE_R, -BIG, BIG)
tunnel_ab = rounded_rect(-TUNNEL_HALF_W, TUNNEL_HALF_W, TUNNEL_BOTTOM, TUNNEL_TOP, TUNNEL_R)
tunnel = to_world(Manifold.extrude(tunnel_ab, BIG).translate([0, 0, -BIG / 2]))
# keep everything in front of the case's back face clear for the case + plug
plug_ab = rounded_rect(-TUNNEL_HALF_W, TUNNEL_HALF_W, TUNNEL_BOTTOM, 0, TUNNEL_R)
front_clear = lcyl(LIP_INNER_R, 0, BIG) + to_world(Manifold.extrude(plug_ab, BIG))

stand = (base + hood + ring) - hole - tunnel - front_clear
stand = (stand ^ world_halfspace(2, 0.0)).simplify(0.002)


# --- magnet pockets ---
def magnet_pockets_at(points):
    """Blind holes in the bottom face at the given (x, y) centres."""
    d, h = MAGNET_D + MAGNET_CLEAR, MAGNET_H + MAGNET_EXTRA
    hole = Manifold.cylinder(h + 1, d / 2, d / 2, 64).translate([0, 0, -1])
    return Manifold.batch_boolean([hole.translate([x, y, 0]) for x, y in points],
                                  OpType.Add)


def magnet_points(solid, step=0.5):
    """Four mirror-symmetric spots, spread as far apart as the base allows,
    each with MAGNET_WALL of plastic around it and MAGNET_TOP above."""
    keep_out = (MAGNET_D + MAGNET_CLEAR) / 2 + MAGNET_WALL
    top = MAGNET_H + MAGNET_EXTRA + MAGNET_TOP
    ok = None
    for z in (0.05, top / 2, top):         # where a pocket centre may go, at every height
        inner = solid.slice(z).offset(-keep_out, JoinType.Round)
        ok = inner if ok is None else ok ^ inner
    (x0, y0), (x1, y1) = ok.bounds()[:2], ok.bounds()[2:]
    dot = CrossSection.circle(0.05, 8)
    widest = {}                             # y -> largest x that fits (mirrored)
    for y in np.arange(y0, y1 + step, step):
        for x in np.arange(x1, keep_out - 1e-9, -step):
            if ((dot.translate([x, y]) ^ ok).area() > 0
                    and (dot.translate([-x, y]) ^ ok).area() > 0):
                widest[y] = x
                break
    ys = sorted(widest)
    best = max(((widest[a] + widest[b]) * (b - a), a, b)
               for i, a in enumerate(ys) for b in ys[i + 1:])
    _, ya, yb = best
    return [(widest[ya], ya), (-widest[ya], ya), (widest[yb], yb), (-widest[yb], yb)]


def add_magnets(solid, points=None):
    if not MAGNETS:
        return solid
    return solid - magnet_pockets_at(points or magnet_points(solid))


stand = add_magnets(stand)


# --- mock-ups used for the clearance check and preview only ---
def device_mock(facet=21.5, body_r=23.0, lip_r=24.0, depth=9.5):
    body_ = lcyl(body_r, 0, depth) + lcyl(lip_r, depth, depth + 2)
    return body_ - lbox(-BIG, BIG, -BIG, -facet, -BIG, BIG)


def plug_mock(port_c=4.0, facet=21.5):
    """Typical up/down 90 deg USB-C head, cable exiting toward the back."""
    head = lbox(-6.25, 6.25, -facet - 11, -facet, port_c - 3.5, port_c + 3.5)
    elbow = lbox(-6.25, 6.25, -facet - 11, -facet - 3, port_c - 14, port_c - 3.5)
    cable = to_world(Manifold.cylinder(60, 2.0, 2.0, 32)
                     .translate([0, -facet - 7, port_c - 74]))
    return head + elbow, cable


if __name__ == "__main__":
    import sys
    out = sys.argv[1] if len(sys.argv) > 1 else "designs/9-automotive.stl"
    mesh = stand.to_mesh()
    tm = trimesh.Trimesh(mesh.vert_properties[:, :3], mesh.tri_verts)
    tm.export(out)
    lo, hi = tm.bounds
    print(f"wrote {out}: {hi[0]-lo[0]:.1f} x {hi[1]-lo[1]:.1f} x {hi[2]-lo[2]:.1f} mm, "
          f"{tm.volume/1000:.1f} cm^3, watertight={tm.is_watertight}")
