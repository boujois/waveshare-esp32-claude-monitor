"""Alternative stand designs that reuse the ring cup from make_stand.py.

Every design keeps the ring exactly where make_stand.py puts it (same height,
same 75 deg angle, same lip and hole) and keeps the same clear space for the
case, the right-angle plug, and the cable running straight back. Only the
structure holding the ring up changes.

    python designs.py            # writes designs/<name>.stl and prints checks
"""

import math
import os

import numpy as np
import trimesh
from manifold3d import CrossSection, JoinType, Manifold, OpType

import make_stand as ms
from make_stand import BIG, N, V, lbox, lcyl, rounded_rect, to_world, world_halfspace

C = np.array([0.0, ms.CENTRE_Y, ms.CENTRE_Z])   # centre of the case's back face


def L(a, b, c):
    """Device-local (a, b, c) -> world point."""
    return np.array([a, 0.0, 0.0]) + C + b * V + c * N


ring_only = ms.ring - lcyl(ms.HOLE_R, -ms.PLATE_T - 1, 1)
keep_clear = ms.front_clear + ms.tunnel


def finish(support, hole_depth=0.0, magnets_at=None):
    """Join a support to the ring and cut the shared clearances."""
    solid = support + ring_only
    if hole_depth:
        solid -= lcyl(ms.HOLE_R, -ms.PLATE_T - hole_depth, 1)
    return ms.add_magnets(((solid - keep_clear) ^ world_halfspace(2, 0.0)).simplify(0.002),
                          magnets_at)


def ellipsoid(centre, radii, seg=48):
    return Manifold.sphere(1.0, seg).scale(list(radii)).translate(list(centre))


def puck(cx, cy, rx, ry, h, edge_r):
    """Flat disc with a rounded top edge, elliptical footprint."""
    prof = (CrossSection.square([rx - edge_r, h])
            + CrossSection.square([rx, h - edge_r])
            + CrossSection.circle(edge_r, 32).translate([rx - edge_r, h - edge_r]))
    m = Manifold.revolve(prof ^ CrossSection.square([rx, h]), 128)
    return m.scale([1, ry / rx, 1]).translate([cx, cy, 0])


def sweep(points, radii, squash=(1.0, 1.0, 1.0), seg=24):
    """Smooth tapered tube: hull of consecutive ellipsoids along a polyline."""
    blobs = [ellipsoid(p, np.array(squash) * r, seg) for p, r in zip(points, radii)]
    return Manifold.batch_boolean(
        [Manifold.batch_hull([a, b]) for a, b in zip(blobs, blobs[1:])], OpType.Add)


def bezier(p0, p1, p2, n=14):
    t = np.linspace(0, 1, n)[:, None]
    return (1 - t) ** 2 * p0 + 2 * (1 - t) * t * p1 + t ** 2 * p2


def bar(p, q, w, ext=0.0):
    """Square-section bar from p to q, sides square to its own axis.

    ext lengthens both ends so they can be trimmed flush afterwards."""
    p, q = np.asarray(p, float), np.asarray(q, float)
    u = (q - p) / np.linalg.norm(q - p)
    s = np.cross([0, 0, 1], u)
    s = s / np.linalg.norm(s) if np.linalg.norm(s) > 1e-6 else np.array([1.0, 0, 0])
    t = np.cross(u, s)
    length = np.linalg.norm(q - p) + 2 * ext
    box = Manifold.cube([w, w, length]).translate([-w / 2, -w / 2, -ext])
    return box.transform([[s[i], t[i], u[i], p[i]] for i in range(3)])


# ---------------------------------------------------------------------------
# 1. Halo (ultra modern): ring floats on two swept arms from a thin puck.
def halo():
    base = puck(0, 14, 34, 34, 5.0, 2.5)
    arms = []
    for side in (-1, 1):
        top = L(side * 16, 12, -2.5)
        foot = np.array([side * 18, 36, 3.5])
        ctrl = np.array([side * 20, 44, 34])
        pts = bezier(foot, ctrl, top)
        radii = np.linspace(5.5, 3.2, len(pts))
        arms.append(sweep(pts, radii, squash=(0.8, 1.0, 1.0)))
    return finish(base + arms[0] + arms[1])


# 2. Pebble (ultra modern, organic): a smooth egg with the ring sunk into it.
def pebble():
    egg = ellipsoid([0, 20, 10], [34, 32, 38], 128)
    return finish(egg)


# 3. Fin (minimal): flat plate plus two thin upright fins, with a solid spine
#    between them that hides the cable in a tunnel; it only shows at the back.
def fin():
    back = 48.0
    plate = Manifold.extrude(rounded_rect(-24, 24, -16, back, 10), 4.5)  # thick enough for magnets
    fins = []
    for side in (-1, 1):
        prof = [L(0, -17, -1)[1:], L(0, -40, -1)[1:], (back, 0), (back, 19), L(0, 10, -1)[1:]]
        f = (Manifold.extrude(CrossSection([[tuple(map(float, p)) for p in prof]]), 4.0)
             .transform([[0, 0, 1, side * 15 - 2], [1, 0, 0, 0], [0, 1, 0, 0]]))
        fins.append(f)
    roof = ms.TUNNEL_TOP + 2.5
    spine = (lbox(-16.9, 16.9, -BIG, roof, -BIG, 0)
             ^ Manifold.cube([BIG, back + BIG / 2, BIG]).translate([-BIG / 2, -BIG / 2, 0]))
    return finish(plate + fins[0] + fins[1] + spine)


# 4. Facet (angular): a low-poly crystal, convex hull of a few hand-placed points.
def facet():
    pts = [
        # footprint
        (-30, -6, 0), (30, -6, 0), (-36, 18, 0), (36, 18, 0),
        (-22, 46, 0), (22, 46, 0), (0, 52, 0), (-14, -14, 0), (14, -14, 0),
        # shoulders
        (-27, 8, 16), (27, 8, 16), (-18, 34, 18), (18, 34, 18),
        # behind the ring
        tuple(L(-15, -12, -2)), tuple(L(15, -12, -2)), tuple(L(0, 4, -3)),
    ]
    return finish(Manifold.hull_points(pts), hole_depth=2.0)


# 5. Trestle (angular, open frame): square bars like a little easel. Legs are
#    trimmed flush into the base frame; the front posts run up into the ring's
#    rim, and the rear legs and crossbar are one mitred frame whose crossbar
#    lies flat on the ring's back, so nothing pokes out.
def trestle():
    w = 5.0
    outline = CrossSection([[(-21.6, -14), (21.6, -14), (20, 46), (-20, 46)]])
    frame = Manifold.extrude(outline - outline.offset(-w, JoinType.Miter), w)
    footprint = Manifold.extrude(outline, BIG)

    legs = []
    for side in (-1, 1):
        # front posts: in the ring's own plane, rising from the side rails
        # straight into the ring's rim (the end is buried in the ring)
        legs.append(bar(L(side * 19, -5, 1.5), L(side * 19, -60, 1.5), w))
    legs = Manifold.batch_boolean(legs, OpType.Add) ^ footprint
    # a round pad in each corner, tangent to both outer edges, to hold a magnet
    pad_r = (ms.MAGNET_D + ms.MAGNET_CLEAR) / 2 + ms.MAGNET_WALL
    corners = outline.offset(-pad_r, JoinType.Miter).to_polygons()[0]
    pads = Manifold.batch_boolean(
        [Manifold.cylinder(w, pad_r, pad_r, 64).translate([x, y, 0]) for x, y in corners],
        OpType.Add)
    return finish(frame + pads + legs + rear_frame(w, footprint),
                  magnets_at=[tuple(map(float, c)) for c in corners])


def rear_frame(w, footprint, top_b=14.0, top_half=14.0, foot=(20.0, 46.0)):
    """Rear legs and crossbar as one mitred U, drawn flat and then swept along
    the ring plane, so the crossbar's front face lies flat on the ring's back
    and every corner is a single continuous surface."""
    a_mid = L(0, top_b, -ms.PLATE_T + 0.5)        # top edge, sunk 0.5 mm into the plate
    f_mid = np.array([0.0, foot[1], 0.0])
    e = f_mid - a_mid
    reach = np.linalg.norm(e)
    e /= reach
    n_plane = np.cross([1.0, 0, 0], e)
    sweep = w / abs(V @ n_plane)                  # sweep length giving w-thick bars
    flare = (foot[0] - top_half) / reach
    ext = reach + 2 * w                           # run the legs past the desk, trim later
    outer = CrossSection([[(-top_half, 0), (top_half, 0),
                           (top_half + flare * ext, ext), (-top_half - flare * ext, ext)]])
    u_frame = outer - outer.offset(-w, JoinType.Miter)
    cols = [np.array([1.0, 0, 0]), e, V * sweep]
    frame3d = Manifold.extrude(u_frame, 1.0).transform(
        [[cols[0][i], cols[1][i], cols[2][i], a_mid[i]] for i in range(3)])
    return frame3d ^ footprint


def finish_solid(body, extra_cut=None, drop=0.0):
    """For designs where the ring is a pocket cut into a solid body.

    drop lowers the finished design and trims that much off the bottom;
    the magnet pockets are cut afterwards so they survive the trim."""
    cut = keep_clear if extra_cut is None else keep_clear + extra_cut
    solid = (body - cut).translate([0, 0, -drop]) ^ world_halfspace(2, 0.0)
    return ms.add_magnets(solid.simplify(0.002))


# 6. Print (made for printing): lies on its flat back on the bed, so the case
#    pocket opens upwards and the cable tunnel and window are vertical shafts.
#    Every outside face leans inwards towards the top: no supports, no bridges,
#    and the front face around the screen is the smooth top layer.
PRINT_DEPTH = 30.0                                 # body depth behind the case
PRINT_DROP = 5.0                                   # sits this much lower than the others


def tombstone(r, bottom, seg=96):
    """Arch-topped outline in the ring plane: round top concentric with the ring."""
    return CrossSection.circle(r, seg) + CrossSection.square([2 * r, -bottom]).translate([-r, bottom])


def print_opt():
    back = to_world(Manifold.extrude(tombstone(31.0, -82), 1.0).translate([0, 0, -PRINT_DEPTH]))
    front = to_world(Manifold.extrude(tombstone(ms.RING_OUTER_R + 2.4, -82), 1.0)
                     .translate([0, 0, ms.LIP_DEPTH - 1]))
    body = Manifold.batch_hull([back, front])
    window = lcyl(ms.HOLE_R, -BIG, 1)
    return finish_solid(body, window, drop=PRINT_DROP)


# 7. Inset: the case sits in a pocket in a solid wedge, its screen flush with
#    the front face. The pocket is open at the top (no overhang above the case)
#    and the cable tunnel has a pointed roof, so it prints base-down unsupported.
def inset():
    r = 2.5                                        # edge rounding
    face = to_world(Manifold.extrude(
        rounded_rect(-31 + r, 31 - r, -75, 4 - r, 6), 1.0).translate([0, 0, 11.5 - r - 1]))
    heel = Manifold.extrude(rounded_rect(-28 + r, 28 - r, 12, 46 - r, 8), 6).translate([0, 0, -6])
    body = Manifold.batch_hull([face, heel]).minkowski_sum(Manifold.sphere(r, 24))
    body = body - lbox(-BIG, BIG, -BIG, BIG, 11.5, BIG)          # flat front face
    body_r, bezel_r = ms.LIP_INNER_R, ms.LIP_INNER_R + 0.5
    pocket = (lcyl(body_r, 0, 9.5) + lcyl(bezel_r, 9.5, BIG)
              + lbox(-body_r, body_r, 0, BIG, 0, 9.5) + lbox(-bezel_r, bezel_r, 0, BIG, 9.5, BIG))
    roof = CrossSection([[(-ms.TUNNEL_HALF_W, ms.TUNNEL_TOP), (ms.TUNNEL_HALF_W, ms.TUNNEL_TOP),
                          (0, ms.TUNNEL_TOP + ms.TUNNEL_HALF_W)]])
    peaked = to_world(Manifold.extrude(roof, BIG).translate([0, 0, -BIG / 2]))
    return finish_solid(body, pocket + peaked)


# 8. Bend: a round column that rises from the desk and bends forward so its
#    end face holds the screen flush. The cable runs through a tunnel inside
#    the column and leaves at the back, so it can't be seen from the front.
def swept_tube(path, tangents, radii, seg=128):
    """Smooth tube: hull of consecutive circular sections along a path."""
    x = np.array([1.0, 0, 0])
    discs = []
    for p, t, r in zip(path, tangents, radii):
        e2 = np.cross(t, x)
        disc = Manifold.cylinder(0.01, r, r, seg).transform(
            [[x[i], e2[i], t[i], p[i]] for i in range(3)])
        discs.append(disc)
    return Manifold.batch_boolean(
        [Manifold.batch_hull([a, b]) for a, b in zip(discs, discs[1:])], OpType.Add)


BEND_R = 28.0          # column radius
BEND_RADIUS = 30.0     # centreline radius of the bend
FOOT_R = 32.0          # slight flare where it meets the desk
FACE = 11.5            # end face is flush with the front of the case


def bend():
    end = C + FACE * N                       # centre of the end face
    straight = 12.0                          # straight run holding the pocket
    arc_end = end - straight * N
    turn = math.radians(ms.SCREEN_ANGLE)     # vertical -> screen normal
    # top of the vertical stem, and the bend's centre in front of it
    stem_top = np.array([0.0, arc_end[1] + BEND_RADIUS * (1 - math.cos(turn)),
                         arc_end[2] - BEND_RADIUS * math.sin(turn)])
    arc_c = stem_top + np.array([0, -BEND_RADIUS, 0])

    pts, tans, rad = [], [], []
    # foot flare then straight stem
    for z in np.linspace(-1.0, stem_top[2], 14):
        k = max(0.0, 1 - (z + 1) / 9.0)
        pts.append(np.array([0, stem_top[1], z])); tans.append(np.array([0, 0, 1.0]))
        rad.append(BEND_R + (FOOT_R - BEND_R) * k * k)
    # bend
    for th in np.linspace(0, turn, 40)[1:]:
        pts.append(arc_c + BEND_RADIUS * np.array([0, math.cos(th), math.sin(th)]))
        tans.append(np.array([0, -math.sin(th), math.cos(th)])); rad.append(BEND_R)
    # straight head, with a soft 1.5 mm roll-off on the front edge
    for d, r in ((straight - 1.5, BEND_R), (straight - 0.6, BEND_R - 0.4),
                 (straight - 0.15, BEND_R - 1.0), (straight, BEND_R - 1.5)):
        pts.append(arc_end + d * N); tans.append(N.copy()); rad.append(r)

    body = swept_tube(np.array(pts), np.array(tans), rad)
    body_r, bezel_r = ms.LIP_INNER_R, ms.LIP_INNER_R + 0.5
    pocket = lcyl(body_r, 0, 9.5) + lcyl(bezel_r, 9.5, BIG)
    return finish_solid(body, pocket)


def automotive():
    """Design 9: the domed stand built by make_stand.py."""
    return ms.stand


DESIGNS = {
    "1-halo": halo,
    "2-pebble": pebble,
    "3-fin": fin,
    "4-facet": facet,
    "5-trestle": trestle,
    "6-print": print_opt,
    "7-inset": inset,
    "8-bend": bend,
    "9-automotive": automotive,
}

# designs meant to be printed in a different orientation: name -> world-to-bed
# rotation (bed = the local back face, case pocket facing up)
PRINT_ON_BACK = {"6-print"}

# designs that sit lower than the shared ring height (mm)
DROPS = {"6-print": PRINT_DROP}


def on_back(m, depth=PRINT_DEPTH):
    """Rotate so the local c axis points up and the back face is on the bed."""
    R = np.array([[1.0, 0, 0], V, N])            # rows: local a, b, c in world
    t = -R @ C
    m = m.transform([[R[i, 0], R[i, 1], R[i, 2], t[i]] for i in range(3)])
    lo = m.bounding_box()[2]
    return m.translate([0, 0, -lo])


# ---------------------------------------------------------------------------
def to_trimesh(m):
    msh = m.to_mesh()
    return trimesh.Trimesh(msh.vert_properties[:, :3], msh.tri_verts)


def cross2(u, v):
    return u[0] * v[1] - u[1] * v[0]


def hull2d(p):
    p = sorted(map(tuple, p))
    def half(pts):
        h = []
        for q in pts:
            while len(h) >= 2 and cross2(np.subtract(h[-1], h[-2]), np.subtract(q, h[-2])) <= 0:
                h.pop()
            h.append(q)
        return h
    lo, hi = half(p), half(p[::-1])
    return np.array(lo[:-1] + hi[:-1])


def inside_margin(poly, q):
    """Distance from q to the nearest edge of convex polygon (negative = outside)."""
    d = []
    for a, b in zip(poly, np.roll(poly, -1, axis=0)):
        e = b - a
        d.append(cross2(e, q - a) / np.linalg.norm(e))
    return min(d)


def check(stand, case_g=40.0, fill=0.5, drop=0.0):
    down = [0, 0, -drop]
    dev = ms.device_mock().translate(down)
    out = {"device": (stand ^ dev).volume()}
    worst = 0.0
    for port_c in (4.0, 6.0):
        head, cable = (m.translate(down) for m in ms.plug_mock(port_c))
        out[f"plug{port_c:.0f}"] = (stand ^ head).volume()
        out["cable"] = (stand ^ cable).volume()
        for d in np.arange(0, 25, 1.0):
            worst = max(worst, (stand ^ (dev + head).translate((N * d).tolist())).volume())
    out["push-in"] = worst
    tm = to_trimesh(stand)
    foot = tm.vertices[tm.vertices[:, 2] < 0.05][:, :2]
    poly = hull2d(foot)
    mass = tm.volume / 1000 * 1.24 * fill
    case_c = C + 5.0 * N + np.array(down)
    com = (tm.center_mass * mass + case_c * case_g) / (mass + case_g)
    out["mass_g"] = mass
    out["com_margin"] = inside_margin(poly, com[:2])
    out["watertight"] = tm.is_watertight
    out["size"] = tm.extents
    return out, tm


def overhang_area(m, limit_deg=45.0):
    """Area of downward faces steeper than limit (excluding the bed)."""
    tm = to_trimesh(m)
    nz = tm.face_normals[:, 2]
    z = tm.triangles_center[:, 2]
    bad = (nz < -math.cos(math.radians(limit_deg))) & (z > 0.3)
    return tm.area_faces[bad].sum()


if __name__ == "__main__":
    os.makedirs("designs", exist_ok=True)
    for name, fn in DESIGNS.items():
        st = fn()
        res, tm = check(st, drop=DROPS.get(name, 0.0))
        tm.export(f"designs/{name}.stl")
        bed = st
        if name in PRINT_ON_BACK:
            bed = on_back(st)
            to_trimesh(bed).export(f"designs/{name}-print-on-back.stl")
        res["overhang"] = overhang_area(bed)
        sx, sy, sz = res["size"]
        print(f"{name:10s} {sx:5.1f} x {sy:5.1f} x {sz:5.1f} mm  ~{res['mass_g']:4.0f} g  "
              f"tip margin {res['com_margin']:5.1f} mm  watertight={res['watertight']}  "
              f"overlaps dev={res['device']:.2f} plug={res['plug4']:.2f}/{res['plug6']:.2f} "
              f"cable={res['cable']:.2f} push-in={res['push-in']:.2f}  "
              f"overhang>45deg={res['overhang']:.0f}mm2")
