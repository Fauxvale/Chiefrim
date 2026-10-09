#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A stand-in for the Skyrim side of the link (docs/DESIGN.md §12, Phase 0).

It maps /dev/shm/chiefrim_v1 as the SKSE plugin will, says hello, sends a
world context (origin and floor at a Skyrim position) and a Teleport, keeps
a heartbeat, and prints the player state Halo publishes, so the Halo side
can be tested without Skyrim.

Usage: tools/fake_skyrim.py [--seconds N] [--x X --y Y --z Z --heading DEG]
  The default position is outside Whiterun's gate in Tamriel.
  --overlay WxH asks Halo for its overlay frames (docs §9) at that size, and
  --grab DIR saves one every few seconds there as a PNG (over a grey
  checkerboard, so transparency shows).

The layout below mirrors protocol/chiefrim_protocol.h; the script checks the
mapping's total_size, which the header pins with static_asserts.
"""

import argparse
import math
import mmap
import os
import struct
import sys
import time

PATH = "/dev/shm/chiefrim_v1"
MAGIC = 0x46454843
VERSION = 24
RING_BYTES = 4 * 1024 * 1024
OFF_DISPLAY = 360 + 2 * (128 + RING_BYTES)
ACTORS_MAX, HITBOXES_MAX = 48, 1024
SLOT_ACTORS_BYTES = 16 + 40 * ACTORS_MAX + 8 + 32 * HITBOXES_MAX
OFF_FRAMES = OFF_DISPLAY + 96 + SLOT_ACTORS_BYTES + 32
OFF_ACTORS = OFF_DISPLAY + 96
FRAME_SLOTS, FRAME_MAX_W, FRAME_MAX_H = 3, 2560, 1440
FRAME_LAYER_BYTES = FRAME_MAX_W * FRAME_MAX_H * 4
FRAME_BYTES = 3 * FRAME_LAYER_BYTES + FRAME_MAX_W * FRAME_MAX_H  # the layers, the weapon share
OFF_CAMERA = OFF_DISPLAY + 24
TOTAL_SIZE = OFF_FRAMES + 192 + FRAME_SLOTS * FRAME_BYTES

# offsets (chiefrim_protocol.h)
SKYRIM_PID, HALO_PID = 16, 20
SKYRIM_STATE, HALO_STATE = 24, 28
SKYRIM_HEARTBEAT, HALO_HEARTBEAT = 32, 36
SLOT_WORLD, SLOT_INPUT, SLOT_PLAYER, SLOT_SKYRIM_PLAYER = 64, 120, 184, 280
RING_TO_HALO, RING_TO_SKYRIM = 360, 360 + 128 + RING_BYTES

SIDE_READY, SIDE_CLOSING = 2, 3
MSG_WRAP, MSG_HELLO, MSG_TELEPORT, MSG_LOG = 0, 1, 2, 3
MSG_HIT_ACTOR, MSG_PLAYER_HURT, MSG_PLAYER_DIED, MSG_GIVE_WEAPON, MSG_KEY_NAMES, MSG_LIGHTING = 6, 7, 8, 9, 10, 11
MSG_CHIEF_STATE, MSG_CHIEF_RESTORE, MSG_CHIEF_HEAL, MSG_EXPLOSION, MSG_FLASHLIGHT = 12, 13, 14, 15, 16
MSG_SHOT = 0x13
MSG_CACHE_PLACE, MSG_CACHE_TAKEN = 0x14, 0x15
SHOTS = [0]
MSG_CONSOLE, MSG_DEBUG = 17, 18
GIVE_LIST, DEBUG_HITBOXES = 1, 1
KIT_HEAD, KIT_WEAPON = "<IIii4BfffII", "<64s2h2hfI"  # cr_chief_state, cr_chief_weapon
POSES = {0: "standing", 1: "crouching", 2: "airborne", 3: "dead"}
MSG_COLLISION_RESET, MSG_COLLISION_TRIS = 4, 5
REGION_UNITS = 1024.0
TRIS_PER_MESSAGE = 1600


ROUGH = False

# --actor-shape: the test actor's hit shapes (cr_hitbox), from its feet, Skyrim
# units; it faces south, towards Chief. (a, b, radius, flags); flag 1: a person's head
HITBOX_HEAD = 1
ACTOR_SHAPES = {
    "person": (128.0, 118.0, [
        ((-8, 0, 12), (-8, 0, 78), 9, 0), ((8, 0, 12), (8, 0, 78), 9, 0),  # legs
        ((0, 0, 82), (0, 0, 104), 17, 0),                                    # body
        ((0, 0, 118), (0, 0, 118), 11, HITBOX_HEAD),                         # head
    ]),
    "wolf": (70.0, 0.0, [
        ((0, 35, 42), (0, -30, 42), 17, 0),                                  # body, nose south
        ((0, -55, 50), (0, -62, 50), 11, 0),                                 # head: a creature's, no headshot
        ((-10, 25, 2), (-10, 25, 32), 5, 0), ((10, 25, 2), (10, 25, 32), 5, 0),
        ((-10, -22, 2), (-10, -22, 32), 5, 0), ((10, -22, 2), (10, -22, 32), 5, 0),
    ]),
}


def actors_payload(frame, actor, shape):
    """cr_actors: one actor (or none), with its hit shapes"""
    if actor is None:
        return struct.pack("<II", frame, 0)
    form_id, flags, x, y, z, heading = actor
    height, head, boxes = ACTOR_SHAPES.get(shape, (128.0, 118.0, []))
    out = struct.pack("<II", frame, 1)
    out += struct.pack("<II3ffffHHI", form_id, flags, x, y, z, heading, height, head, 0, len(boxes), 0)
    out += bytes(40 * (ACTORS_MAX - 1))
    out += struct.pack("<II", len(boxes), 0)
    for (ax, ay, az), (bx, by, bz), radius, box_flags in boxes:
        out += struct.pack("<3f3ffI", x + ax, y + ay, z + az, x + bx, y + by, z + bz, radius, box_flags)
    return out


def terrain_height(dx, dy):
    """the synthetic ground, relative to the start (Skyrim units)"""
    h = 0.0
    if ROUGH:  # gentle bumps: every triangle its own plane
        h += 12 * math.sin(dx * 0.013) * math.cos(dy * 0.011) + 6 * math.sin((dx + dy) * 0.031)
    if dy > 256:  # 25 degree ramp north, to a plateau
        h += min(dy - 256, 1244) * math.tan(math.radians(25))
    if dx > 600:  # 60 degree cliff east
        h += (dx - 600) * math.tan(math.radians(60))
    return h


HOLE = False
GROUND_COUNT = 0
PRINT_EVERY = float(os.environ.get("FAKE_PRINT_EVERY", "0.25"))
SOLID = set()  # indices of closed shapes' faces (one-sided, wound outward)


def add_box(tris, corners):
    """a box's 12 triangles, wound outward from its centre, marked solid"""
    centre = [sum(c[k] for c in corners) / 8 for k in range(3)]
    for q in ((0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)):
        a0, a1, a2, a3 = (corners[i] for i in q)
        for t in (a0 + a1 + a2, a0 + a2 + a3):
            e1 = [t[3 + k] - t[k] for k in range(3)]
            e2 = [t[6 + k] - t[k] for k in range(3)]
            n = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
            mid = [(t[k] + t[3 + k] + t[6 + k]) / 3 - centre[k] for k in range(3)]
            if sum(n[k] * mid[k] for k in range(3)) < 0:
                t = t[0:3] + t[6:9] + t[3:6]
            SOLID.add(len(tris))
            tris.append(t)
CR_TRIANGLE_ONE_SIDED = 0x0001
CR_TRIANGLE_LAND = 0x0002
LEDGE = 0.0
PLANK = False
ROOF = 0.0  # a slab this high over where the actor stands (--roof)
ROOF_Y = 0.0


def terrain_triangles(ox, oy, oz):
    """(triangle as 9 floats) list, world coordinates, wound counter-clockwise from above"""
    tris = []
    SOLID.clear()
    step, half = 64.0, 3072.0
    n = int(2 * half / step)
    for i in range(n):
        for j in range(n):
            x0, y0 = -half + i * step, -half + j * step
            x1, y1 = x0 + step, y0 + step
            if HOLE and abs(x0 + step / 2) < 200 and abs(y0 + step / 2) < 200:
                continue  # a hole in the ground where the player starts
            p = [(x0, y0), (x1, y0), (x1, y1), (x0, y1)]
            v = [(ox + x, oy + y, oz + terrain_height(x, y)) for x, y in p]
            tris.append(v[0] + v[1] + v[2])
            tris.append(v[0] + v[2] + v[3])
    global GROUND_COUNT
    GROUND_COUNT = len(tris)  # the land: one-sided, as the plugin flags Skyrim's
    if ROUGH:  # rocks: boxes, rotated, scattered (not near the start)
        import random
        rng = random.Random(7)
        for _ in range(400):
            cx, cy = rng.uniform(-3000, 3000), rng.uniform(-3000, 3000)
            if abs(cx) < 300 and abs(cy) < 300:
                continue
            hx, hy, hz, a = rng.uniform(20, 120), rng.uniform(20, 120), rng.uniform(10, 80), rng.uniform(0, math.pi)
            base = oz + terrain_height(cx, cy)
            corners = []
            for k in range(8):
                lx, ly, lz = (hx if k & 1 else -hx), (hy if k & 2 else -hy), (hz if k & 4 else -hz)
                corners.append((ox + cx + lx * math.cos(a) - ly * math.sin(a), oy + cy + lx * math.sin(a) + ly * math.cos(a), base + lz))
            add_box(tris, corners)
    if LEDGE:  # a slab like a road piece across the way north, LEDGE units up, from y 80 to 200
        corners = [(ox + (300 if k & 1 else -300), oy + (200 if k & 2 else 80), oz + (LEDGE if k & 4 else (LEDGE - 4 if PLANK else -10))) for k in range(8)]
        add_box(tris, corners)
    if ROOF:  # a slab over the actor's spot, ROOF units up, 20 thick: a shadow from above
        corners = [(ox + (250 if k & 1 else -250), oy + ROOF_Y + (250 if k & 2 else -250), oz + (ROOF + 20 if k & 4 else ROOF)) for k in range(8)]
        add_box(tris, corners)
    # a wall south of the start, 400 high, facing north
    a, b = (ox - 2000, oy - 800, oz), (ox + 2000, oy - 800, oz)
    c, d = (ox + 2000, oy - 800, oz + 400), (ox - 2000, oy - 800, oz + 400)
    tris.append(a + b + c)
    tris.append(a + c + d)
    return tris


DUMP = None  # (triangles with flags, Chief's position), world units about the dump's origin


def load_dump(path):
    """a build Halo dumped: its triangles (world units, with their flags) and where Chief was"""
    data = open(path, "rb").read()
    count = struct.unpack_from("<i", data, 8)[0]
    tris = []
    for i in range(count):
        v = struct.unpack_from("<9fIhh", data, 12 + 44 * i)
        tris.append((v[0:9], v[11] & 0xFFFF))
    off = 12 + 44 * count
    chief = struct.unpack_from("<3f", data, off + 4) if len(data) >= off + 16 and struct.unpack_from("<i", data, off)[0] else (0.0, 0.0, 0.0)
    return tris, chief


def world_triangles(ox, oy, oz):
    """(triangle, flags): a dump's, about the origin, or the synthetic terrain"""
    if DUMP:
        k = 213.36
        return [(tuple(c * k + (ox, oy, oz)[j % 3] for j, c in enumerate(t)), flags) for t, flags in DUMP[0]]
    return [(t, (CR_TRIANGLE_ONE_SIDED | CR_TRIANGLE_LAND) if i < GROUND_COUNT else CR_TRIANGLE_ONE_SIDED if i in SOLID else 0)
            for i, t in enumerate(terrain_triangles(ox, oy, oz))]


def send_terrain(link, epoch, ox, oy, oz, generation=1):
    regions = {}
    for t, flags in world_triangles(ox, oy, oz):
        cx, cy, cz = (t[0] + t[3] + t[6]) / 3, (t[1] + t[4] + t[7]) / 3, (t[2] + t[5] + t[8]) / 3
        key = (math.floor(cx / REGION_UNITS), math.floor(cy / REGION_UNITS), math.floor(cz / REGION_UNITS))
        regions.setdefault(key, []).append((t, flags))
    link.push(RING_TO_HALO, MSG_COLLISION_RESET, struct.pack("<II", epoch, generation))
    sent = 0
    for (rx, ry, rz), tris in regions.items():
        for first in range(0, max(len(tris), 1), TRIS_PER_MESSAGE):
            chunk = tris[first:first + TRIS_PER_MESSAGE]
            body = struct.pack("<IiiiIIII", epoch, rx, ry, rz, len(tris), first, len(chunk), 0)
            body += b"".join(struct.pack("<9fHH", *t, 0, flags) for t, flags in chunk)
            while not link.push(RING_TO_HALO, MSG_COLLISION_TRIS, body):
                time.sleep(0.01)
            link.set_u32(SKYRIM_HEARTBEAT, int(time.monotonic() * 1000))  # still here
            sent += len(chunk)
    print(f"fake_skyrim: terrain: {sent} triangles in {len(regions)} regions", flush=True)
ACTION_JUMP, ACTION_CROUCH = 0, 1
INPUT_FORMAT = "<IIII16Bffdd"  # cr_input (protocol v2)

# (start s, end s, what): the --drive script, after a 2 s settle
DRIVE = [(2, 4, "forward"), (4, 6, "strafe right"), (6, 8, "turn right"),
         (8, 8.1, "jump"), (10, 12, "crouch"), (12, 13, "forward"),
         (13, 15, "stall"), (15, 16, "stop")]
# --terrain --drive: up the ramp north, east into the cliff, south down to the wall
DRIVE_TERRAIN = [(3, 7, "forward"), (7, 7.05, "turn right 90"), (7.5, 11, "forward"),
                 (11, 11.05, "turn right 90"), (11.5, 18, "forward"), (18, 19, "stop")]


def drive_input(t, frame, presses, state):
    """the cr_input for t seconds into the run"""
    forward = strafe = 0.0
    held = 0
    plan = DRIVE_TERRAIN if state.get("terrain") else DRIVE
    phase = next((what for start, end, what in plan if start <= t < end), "stop")
    if state.get("walk"):
        phase = "walk" if t >= 2 else "stop"
        if phase == "walk":
            forward = 1.0
            state["yaw"] += 0.0015  # ~0.3 rad/s: wander over what's there
    if phase == "forward":
        forward = 1.0
    elif phase == "strafe right":
        strafe = 1.0
    elif phase == "turn right":
        state["yaw"] += 0.02  # radians per tick of this loop (~200/s): ~4 rad/s
    elif phase == "turn right 90" and state.get("turned") != t // 1:
        state["yaw"] += math.pi / 2
        state["turned"] = t // 1
    elif phase == "jump":
        held |= 1 << ACTION_JUMP
    elif phase == "crouch":
        held |= 1 << ACTION_CROUCH
    if held & (1 << ACTION_JUMP) and not state.get("jumped"):
        presses[ACTION_JUMP] = (presses[ACTION_JUMP] + 1) & 0xFF
        state["jumped"] = True
    if state.get("phase") != phase:
        print(f"fake_skyrim: drive: {phase}", flush=True)
        state["phase"] = phase
    return struct.pack(INPUT_FORMAT, frame, 0, held, 1, *presses, forward, strafe, state["yaw"], 0.0)
TAMRIEL = 0x3C


class Link:
    def __init__(self, shm):
        self.shm = shm

    def u32(self, offset):
        return struct.unpack_from("<I", self.shm, offset)[0]

    def set_u32(self, offset, value):
        struct.pack_into("<I", self.shm, offset, value & 0xFFFFFFFF)

    def slot_write(self, offset, payload):
        seq = self.u32(offset)
        self.set_u32(offset, seq + 1)
        self.shm[offset + 8:offset + 8 + len(payload)] = payload
        self.set_u32(offset, seq + 2)

    def slot_read(self, offset, size):
        for _ in range(64):
            before = self.u32(offset)
            if before & 1:
                continue
            payload = bytes(self.shm[offset + 8:offset + 8 + size])
            if self.u32(offset) == before:
                return before, payload
        return 0, None

    def push(self, ring, msg_type, body):
        size = 8 + len(body)
        total = (size + 7) & ~7
        head, tail = self.u32(ring), self.u32(ring + 64)
        offset = head % RING_BYTES
        room = RING_BYTES - offset
        needed = total + (room if room < total else 0)
        if RING_BYTES - ((head - tail) & 0xFFFFFFFF) < needed:
            return False
        data = ring + 128
        if room < total:
            if room >= 8:
                struct.pack_into("<HHI", self.shm, data + offset, MSG_WRAP, 0, 0)
            head += room
            offset = 0
        message = struct.pack("<HHI", msg_type, total, 0) + body
        self.shm[data + offset:data + offset + len(message)] = message
        self.set_u32(ring, head + total)
        return True

    def pop(self, ring):
        data = ring + 128
        while True:
            tail, head = self.u32(ring + 64), self.u32(ring)
            if head == tail:
                return None
            offset = tail % RING_BYTES
            room = RING_BYTES - offset
            if room < 8:
                self.set_u32(ring + 64, tail + room)
                continue
            msg_type, size, _ = struct.unpack_from("<HHI", self.shm, data + offset)
            if msg_type == MSG_WRAP and size == 0:
                self.set_u32(ring + 64, tail + room)
                continue
            body = bytes(self.shm[data + offset + 8:data + offset + size])
            self.set_u32(ring + 64, tail + size)
            return msg_type, body


def kit_unpack(body):
    head = struct.unpack_from(KIT_HEAD, body)
    weapons = [struct.unpack_from(KIT_WEAPON, body, 40 + 80 * i) for i in range(4)]
    return {"generation": head[0], "flags": head[1], "current": head[2], "grenade": head[3],
            "grenades": list(head[4:8]), "body": head[8], "shield": head[9], "flashlight": head[10],
            "weapons": [{"tag": text(w[0]), "total": [w[1], w[2]], "loaded": [w[3], w[4]], "age": w[5]} for w in weapons]}


def kit_pack(kit):
    body = struct.pack(KIT_HEAD, kit["generation"], kit["flags"], kit["current"], kit["grenade"],
                       *kit["grenades"], kit["body"], kit["shield"], kit["flashlight"], 0, 0)
    for w in kit["weapons"]:
        body += struct.pack(KIT_WEAPON, w["tag"].encode()[:63], *w["total"], *w["loaded"], w["age"], 0)
    return body


def kit_text(kit):
    weapons = ", ".join(f"{w['tag'].rsplit(chr(92), 1)[-1]} {w['loaded'][0]}/{w['total'][0]}"
                        + (" (in hand)" if i == kit["current"] else "")
                        for i, w in enumerate(kit["weapons"]) if w["tag"])
    return (f"gen {kit['generation']}: {weapons or 'no weapons'}; grenades {kit['grenades'][:2]} (type {kit['grenade']}); "
            f"body {kit['body']:.2f}, shields {kit['shield']:.2f}, flashlight {kit['flashlight']:.2f}")


def text(raw):
    return raw.split(b"\0", 1)[0].decode(errors="replace")


def covered_mean(pixels):
    """mean brightness (0-255) of a layer's covered pixels (alpha over 200), and how many"""
    total = count = 0
    for i in range(0, len(pixels), 4 * 7):  # every 7th pixel: enough, and quick
        if pixels[i + 3] > 200:
            total += pixels[i] + pixels[i + 1] + pixels[i + 2]
            count += 1
    return (total / (3 * count) if count else 0.0), count


def grab_frame(link, directory, index):
    """The latest overlay frame (cr_frames) as a PNG over a checkerboard; False if none or torn."""
    import zlib
    latest = link.u32(OFF_FRAMES)
    if not latest:
        return False
    slot = latest - 1
    header = OFF_FRAMES + 32 + 48 * slot
    seq, width, height, frame, camera_frame, time_us, flags, tangent_x, tangent_y = struct.unpack_from("<7I2f", link.shm, header)
    if seq & 1 or not (0 < width <= FRAME_MAX_W and 0 < height <= FRAME_MAX_H):
        return False
    start = OFF_FRAMES + 192 + slot * FRAME_BYTES
    pixels = bytes(link.shm[start:start + width * height * 4])
    mask = bytes(link.shm[start + 3 * FRAME_LAYER_BYTES:start + 3 * FRAME_LAYER_BYTES + width * height]) if flags & 4 else None
    world = bytes(link.shm[start + FRAME_LAYER_BYTES:start + FRAME_LAYER_BYTES + width * height * 4]) if flags & 2 else None
    depth = bytes(link.shm[start + 2 * FRAME_LAYER_BYTES:start + 2 * FRAME_LAYER_BYTES + width * height * 4]) if flags & 2 else None
    if link.u32(header) != seq:
        return False
    saved = [write_png(directory, f"overlay{index:03}.png", width, height, pixels)]
    mean, count = covered_mean(pixels)
    note = f"; screen layer {mean:.0f} bright on {count}"
    if mask:
        saved.append(write_png(directory, f"weapon{index:03}.png", width, height, b"".join(bytes((m, m, m, 255)) for m in mask)))
        weapon = sum(1 for i in range(0, len(mask), 7) if mask[i] > 127)
        note += f" ({weapon} of them the weapon's)"
    if world:
        saved.append(write_png(directory, f"world{index:03}.png", width, height, world))
        mean, count = covered_mean(world)
        note += f", world layer {mean:.0f} on {count}"
        values = [v for v in struct.unpack(f"<{width * height}f", depth) if v < 1e29]
        if values:
            note += (f"; world depth on {100.0 * len(values) / (width * height):.1f}%: {min(values) * 213.36:.0f}"
                    f"-{max(values) * 213.36:.0f} Skyrim units")
    print(f"fake_skyrim: frame {frame} ({width}x{height}, flags {flags}, camera {camera_frame} of"
          f" {LAST_CAMERA[0]}, tangents {tangent_x:.4f} x {tangent_y:.4f}{note}) -> {', '.join(saved)}", flush=True)
    return True


LAST_CAMERA = [0]


def write_png(directory, name, width, height, pixels):
    """premultiplied RGBA over a grey checkerboard, as a PNG"""
    import zlib
    rows = []
    covered = 0
    for y in range(height):
        row = bytearray(b"\0")
        line = pixels[y * width * 4:(y + 1) * width * 4]
        for x in range(width):
            r, g, b, a = line[x * 4:x * 4 + 4]
            if a:
                covered += 1
            grey = 150 if ((x >> 4) + (y >> 4)) & 1 else 100
            keep = 255 - a
            row += bytes((min(255, r + grey * keep // 255), min(255, g + grey * keep // 255), min(255, b + grey * keep // 255)))
        rows.append(bytes(row))
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(b"".join(rows), 6)) + chunk(b"IEND", b"")
    path = os.path.join(directory, name)
    with open(path, "wb") as out:
        out.write(png)
    return f"{path} ({100.0 * covered / (width * height):.1f}% covered)"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--seconds", type=float, default=60)
    parser.add_argument("--x", type=float, default=19500.0)
    parser.add_argument("--y", type=float, default=-7400.0)
    parser.add_argument("--z", type=float, default=-3650.0)
    parser.add_argument("--heading", type=float, default=0.0, help="degrees, 0 = north")
    parser.add_argument("--fov", type=float, default=85.0, help="Chief's field of view, as Skyrim measures it (0: Halo's own)")
    parser.add_argument("--silence-at", type=float, default=0.0,
                        help="seconds in: stop the heartbeat, as a Skyrim that hangs (0: never)")
    parser.add_argument("--silence-for", type=float, default=5.0)
    parser.add_argument("--terrain", action="store_true",
                        help="stream synthetic collision: a 25 degree ramp up north, a 60 degree cliff east, a wall south;"
                             " with --drive, walk into each")
    parser.add_argument("--rough", action="store_true",
                        help="with --terrain: bumpy ground (a plane per triangle, like Skyrim's) and boxes like rocks")
    parser.add_argument("--drive", action="store_true",
                        help="script Chief through the input slot: forward, strafe right, turn right, jump, crouch")
    parser.add_argument("--recenter-every", type=float, default=0.0,
                        help="seconds: move the world origin to Chief, as Skyrim does on a load or a door"
                             " (a new world context, a Teleport where he is, the collision again)")
    parser.add_argument("--start-below", type=float, default=0.0,
                        help="Skyrim units: start the player (and the floor) this far under the terrain,"
                             " as when Skyrim's ground is higher than the stand-in floor")
    parser.add_argument("--ledge", type=float, default=0.0,
                        help="with --terrain: a slab this many units high across the way north, like a road piece (the step assist)")
    parser.add_argument("--plank", action="store_true",
                        help="with --ledge: the slab is a board 4 units thick, open below (a boardwalk's edge)")
    parser.add_argument("--sink-at", type=float, default=0.0,
                        help="seconds in: put Chief 100 units under the ground (a test of the land rule)")
    parser.add_argument("--dump", default="",
                        help="the world is a collision dump Halo saved (build/collision-dumps), Chief starting where it says")
    parser.add_argument("--walk", action="store_true",
                        help="walk forward all the time, turning slowly")
    parser.add_argument("--skyrim-moves", action="store_true",
                        help="as the plugin does by default: the fake's own player walks north over the terrain and Halo's Chief follows")
    parser.add_argument("--hole", action="store_true",
                        help="with --terrain: no ground under the start, so Chief falls through (a test of the catch)")
    parser.add_argument("--radius", type=float, default=0.0,
                        help="Chief's collision radius in Skyrim units, as the plugin sends it (0: Halo's own)")
    parser.add_argument("--height", type=float, default=128.0,
                        help="Chief's height in Skyrim units, as the plugin sends it (0: Halo's own)")
    parser.add_argument("--overlay", default="",
                        help="WxH: ask Halo for overlay frames at this size, as the plugin does (docs §9)")
    parser.add_argument("--grab", default="",
                        help="with --overlay: save a frame to this directory every --grab-every seconds")
    parser.add_argument("--grab-every", type=float, default=5.0)
    parser.add_argument("--actor", type=float, default=0.0,
                        help="Skyrim units: a hostile actor (128 tall) stands this far north of the start; Halo's hits on it are printed")
    parser.add_argument("--actor-shape", choices=("biped",) + tuple(ACTOR_SHAPES), default="biped",
                        help="with --actor: its hit shapes (protocol 18); biped: none, Halo hits its proxy's biped")
    parser.add_argument("--hurt-at", type=float, default=0.0,
                        help="seconds in: the player is hurt, --hurt-count times a second apart (melee, from the north)")
    parser.add_argument("--hurt-amount", type=float, default=0.2, help="each hurt, of Chief's whole vitality")
    parser.add_argument("--hurt-count", type=int, default=3)
    parser.add_argument("--give-at", type=float, default=0.0,
                        help="seconds in: give Chief the host map's next weapon, --give-count times a second apart")
    parser.add_argument("--give-count", type=int, default=1)
    parser.add_argument("--cache", default="",
                        help="a weapon cache: this weapon laid down 150 units north of the start, sent every 2 s as the plugin does")
    parser.add_argument("--cache-at", type=float, default=2.0)
    parser.add_argument("--cache-north", type=float, default=150.0, help="how far north of the start the cache lies")
    parser.add_argument("--action-at", type=float, default=0.0,
                        help="seconds in: hold the action key (pick up, swap) for 2 s")
    parser.add_argument("--give-name", default="",
                        help="with --give-at: weapons by name, comma-separated, in turn (the console's chiefrim give); empty: the next")
    parser.add_argument("--list-weapons-at", type=float, default=0.0, help="seconds in: the console's chiefrim weapons")
    parser.add_argument("--shapes", action="store_true", help="the console's chiefrim shapes on: Halo draws the proxies' hit shapes")
    parser.add_argument("--restore-at", type=float, default=0.0,
                        help="seconds in: as a save's load, send back Chief's last kit changed "
                             "(weapons in reverse, the last in hand, half their rounds, 3 frags and 2 plasmas, "
                             "body 0.5, shields 0.25) (CR_MSG_CHIEF_RESTORE)")
    parser.add_argument("--heal-at", type=float, default=0.0,
                        help="seconds in: a potion heals --heal-amount of Chief's whole vitality (CR_MSG_CHIEF_HEAL)")
    parser.add_argument("--heal-amount", type=float, default=0.2)
    parser.add_argument("--loadout-at", type=float, default=0.0,
                        help="seconds in: as Chiefrim.ini's [Loadout], --loadout's weapons by name with their own rounds")
    parser.add_argument("--loadout", default="shotgun, Sniper Rifle",
                        help="comma-separated weapon names (a tag path's last part)")
    parser.add_argument("--restore-default-at", type=float, default=0.0,
                        help="seconds in: as a save without a kit: the starting loadout")
    parser.add_argument("--light", type=float, default=-1.0,
                        help="Skyrim's light for Halo's objects (CR_MSG_LIGHTING): ambient and a sun from above, this bright (0: dark)")
    parser.add_argument("--light-to", type=float, default=-1.0, help="with --light: this bright from --light-at seconds in")
    parser.add_argument("--light-at", type=float, default=10.0)
    parser.add_argument("--sun-color", default="1,0.95,0.85",
                        help="with --light: the sun's colour (r,g,b), scaled by --light")
    parser.add_argument("--torch", default="",
                        help="with --light: a point light 'dx,dy,dz,reach,r,g,b' from the start (Skyrim units), as a torch on a wall")
    parser.add_argument("--sun-visible", type=float, default=-1.0,
                        help="with --light: shadows on (protocol 21), and this much of the sun reaches Chief's eye (0..1)")
    parser.add_argument("--sky-visible", type=float, default=1.0,
                        help="with --sun-visible: how much of the sky is open over Chief's eye (0: under a roof)")
    parser.add_argument("--sun-visible-to", type=float, default=-1.0, help="with --sun-visible: this from --sun-visible-at seconds in")
    parser.add_argument("--sun-visible-at", type=float, default=10.0)
    parser.add_argument("--roof", type=float, default=0.0,
                        help="with --terrain: a slab this high over --actor's spot (its shadow from above)")
    parser.add_argument("--key-names", default="",
                        help="comma-separated, per CR_ACTION_* (jump,crouch,fire,zoom,reload,grenade,melee,action,...): "
                             "the keys Halo's prompts show")
    parser.add_argument("--collision-radius", type=int, default=2,
                        help="regions around Chief's that Halo builds its collision from, as the plugin sends it")
    parser.add_argument("--speed", type=float, default=300.0,
                        help="with --skyrim-moves: units per second the player walks north (0: stands)")
    parser.add_argument("--pitch", type=float, default=0.0,
                        help="with --skyrim-moves: degrees the player looks up (negative: down)")
    parser.add_argument("--fire-at", type=float, default=0.0,
                        help="seconds in: hold fire for --fire-for seconds (bullets, casings, decals: the world layer)")
    parser.add_argument("--fire-for", type=float, default=3.0)
    parser.add_argument("--grenade-at", type=float, default=0.0,
                        help="seconds in: throw a grenade (an explosion's hits on --actor say so)")
    parser.add_argument("--zoom-at", type=float, default=0.0,
                        help="seconds in: switch weapon (to the pistol, on b30), then hold zoom from 2 s later"
                             " (without --drive: the input slot is otherwise unused)")
    parser.add_argument("--actor-friendly", action="store_true",
                        help="with --actor: not hostile to the player (a yellow blip on the motion tracker, not red)")
    parser.add_argument("--actor-walk", type=float, default=0.0,
                        help="with --actor: Skyrim units a second it walks east and west, 400 units each way (the motion tracker)")
    parser.add_argument("--actor-sneaks", action="store_true",
                        help="with --actor: sneaking (CR_ACTOR_SNEAKING: only Halo's own speed shows it on the tracker)")
    parser.add_argument("--actor-attack-at", type=float, default=0.0,
                        help="with --actor: seconds in, it attacks for 3 s (CR_ACTOR_ATTACKING: the tracker shows it standing)")
    parser.add_argument("--actor-dies-at", type=float, default=0.0,
                        help="with --actor: seconds in, it dies (listed dead for 3 s, then gone): its proxy drops its kit")
    parser.add_argument("--flashlight-at", type=float, default=0.0,
                        help="seconds in: switch Chief's flashlight on, and off again 4 s later")
    options = parser.parse_args()
    if options.dump:
        global DUMP
        DUMP = load_dump(options.dump)
        options.terrain = True  # send it
        # Chief's start: the dump's own spot, the origin where the dump's was
        options.x, options.y, options.z = 19500.0 + DUMP[1][0] * 213.36, -7400.0 + DUMP[1][1] * 213.36, -3650.0 + DUMP[1][2] * 213.36
        options.dump_origin = (19500.0, -7400.0, -3650.0)
    if options.walk:
        options.drive = True

    print(f"fake_skyrim: waiting for {PATH}", flush=True)
    deadline = time.monotonic() + options.seconds
    while True:
        try:
            fd = os.open(PATH, os.O_RDWR)
            if os.fstat(fd).st_size >= TOTAL_SIZE:
                shm = mmap.mmap(fd, TOTAL_SIZE)
                os.close(fd)
                link = Link(shm)
                if link.u32(0) == MAGIC:
                    break
                shm.close()
            else:
                os.close(fd)
        except FileNotFoundError:
            pass
        if time.monotonic() > deadline:
            print("fake_skyrim: Halo never appeared")
            return 1
        time.sleep(0.2)

    if link.u32(4) != VERSION or link.u32(8) != TOTAL_SIZE:
        print(f"fake_skyrim: version {link.u32(4)}, size {link.u32(8)}: layout mismatch")
        return 1
    print(f"fake_skyrim: mapped, Halo pid {link.u32(HALO_PID)}", flush=True)

    link.set_u32(SKYRIM_PID, os.getpid())
    link.set_u32(SKYRIM_STATE, SIDE_READY)
    link.push(RING_TO_HALO, MSG_HELLO,
        struct.pack("<II48s", VERSION, os.getpid(), b"fake_skyrim.py"))
    # cr_world_context: world_id, is_interior, origin, floor_z, generation, field_of_view, chief_height, reserved
    start_z = options.z - options.start_below
    link.slot_write(SLOT_WORLD, struct.pack("<II3ffIfffII",
        TAMRIEL, 0, options.x, options.y, start_z, start_z, 1, options.fov, options.height, options.radius, options.collision_radius, 0))
    link.push(RING_TO_HALO, MSG_TELEPORT,
        struct.pack("<4f", options.x, options.y, start_z, math.radians(options.heading)))
    if options.terrain:
        global ROUGH, HOLE, LEDGE
        ROUGH = options.rough
        HOLE = options.hole
        LEDGE = options.ledge
        global PLANK, ROOF, ROOF_Y
        PLANK = options.plank
        ROOF, ROOF_Y = options.roof, options.actor
        send_terrain(link, 1, *(options.dump_origin if DUMP else (options.x, options.y, options.z)))

    display = None
    if options.overlay:
        w, h = (int(v) for v in options.overlay.lower().split("x"))
        display = [w, h, 0]
    if options.grab:
        os.makedirs(options.grab, exist_ok=True)
    last_grab, grabs = time.monotonic(), 0
    last_print = 0.0
    last_seq = 0
    last_tick = None
    started = time.monotonic()
    silenced = False
    frame = 0
    presses = [0] * 16
    drive_state = {"yaw": 0.0, "terrain": options.terrain, "walk": options.walk}
    generation, epoch = 1, 1
    last_recenter = time.monotonic()
    last_position = None
    try:
        while time.monotonic() < deadline:
            quiet = options.silence_at > 0 and 0 <= time.monotonic() - started - options.silence_at < options.silence_for
            if quiet != silenced:
                print(f"fake_skyrim: heartbeat {'stopped' if quiet else 'back'}", flush=True)
                silenced = quiet
            if not quiet:
                link.set_u32(SKYRIM_HEARTBEAT, int(time.monotonic() * 1000))
            if options.drive:
                t = time.monotonic() - started
                stalled = any(start <= t < end and what == "stall" for start, end, what in (DRIVE_TERRAIN if options.terrain else DRIVE))
                if stalled and drive_state.get("phase") != "stall":
                    print("fake_skyrim: drive: stall (input stops, forward was held; Chief should stop)", flush=True)
                    drive_state["phase"] = "stall"
                if not stalled:  # a stalled Skyrim publishes nothing, heartbeat or not
                    frame += 1
                    link.slot_write(SLOT_INPUT, drive_input(t, frame, presses, drive_state))
            while (message := link.pop(RING_TO_SKYRIM)) is not None:
                msg_type, body = message
                if msg_type == MSG_HELLO:
                    version, pid = struct.unpack_from("<II", body)
                    print(f"fake_skyrim: Halo says hello (protocol {version}, pid {pid}, {text(body[8:56])})", flush=True)
                elif msg_type == MSG_LOG:
                    print(f"halo: {text(body)}", flush=True)
                elif msg_type == MSG_CONSOLE:
                    print(f"console: {text(body)}", flush=True)
                elif msg_type == MSG_HIT_ACTOR:
                    form_id, flags, fraction, _, bx, by, bz = struct.unpack_from("<IIff3f", body)
                    blast = f", explosion at ({bx:.0f} {by:.0f} {bz:.0f})" if flags & 1 else ""
                    blast += ", headshot" if flags & 2 else ""
                    print(f"fake_skyrim: Chief hit actor {form_id:08X} for {fraction:.3f} of its proxy{blast}", flush=True)
                elif msg_type == MSG_PLAYER_DIED:
                    print("fake_skyrim: Chief died: Skyrim's player would die now", flush=True)
                elif msg_type == MSG_EXPLOSION:
                    cx, cy, cz, radius, acceleration = struct.unpack_from("<3fff", body)
                    print(f"fake_skyrim: explosion at ({cx:.0f} {cy:.0f} {cz:.0f}), radius {radius:.0f}, acceleration {acceleration:.3f}", flush=True)
                elif msg_type == MSG_CACHE_TAKEN:
                    print(f"fake_skyrim: cache {struct.unpack_from('<I', body)[0]:08X} taken", flush=True)
                elif msg_type == MSG_SHOT:
                    fx, fy, fz, tx, ty, tz = struct.unpack_from("<3f3f", body)
                    SHOTS[0] += 1
                    if SHOTS[0] <= 3 or SHOTS[0] % 50 == 0:
                        print(f"fake_skyrim: shot #{SHOTS[0]}: a projectile's way ({fx:.0f} {fy:.0f} {fz:.0f}) -> ({tx:.0f} {ty:.0f} {tz:.0f}), "
                              f"{math.dist((fx, fy, fz), (tx, ty, tz)):.0f} units", flush=True)
                elif msg_type == MSG_FLASHLIGHT:
                    r, g, b, radius, cutoff, falloff = struct.unpack_from("<3ffff", body)
                    state = f"on, colour ({r:.2f} {g:.2f} {b:.2f})" if r + g + b > 0 else "off"
                    print(f"fake_skyrim: Chief's flashlight {state}, radius {radius:.0f}, cone {math.degrees(cutoff):.0f} "
                          f"(full to {math.degrees(falloff):.0f}) degrees", flush=True)
                elif msg_type == MSG_CHIEF_STATE:
                    kit = kit_unpack(body)
                    shown = kit_text(dict(kit, body=round(kit["body"], 1), shield=round(kit["shield"], 1)))
                    if shown != drive_state.get("kit_shown"):
                        print(f"fake_skyrim: Chief's kit {kit_text(kit)}", flush=True)
                        drive_state["kit_shown"] = shown
                    drive_state["kit"] = kit
            if (options.recenter_every > 0 and last_position and
                    time.monotonic() - last_recenter >= options.recenter_every):
                # wherever a build is: the race between a build and a new origin
                px, py, pz, yaw = last_position
                generation += 1
                epoch += 1
                link.slot_write(SLOT_WORLD, struct.pack("<II3ffIfffII",
                    TAMRIEL, 0, px, py, pz, pz, generation, options.fov, options.height, options.radius, options.collision_radius, 0))
                link.push(RING_TO_HALO, MSG_TELEPORT, struct.pack("<4f", px, py, pz + 5.0, yaw))
                print(f"fake_skyrim: recenter #{generation - 1} at ({px:.1f} {py:.1f} {pz:.1f})", flush=True)
                if options.terrain:
                    send_terrain(link, epoch, *(options.dump_origin if DUMP else (options.x, options.y, options.z)), generation)
                last_recenter = time.monotonic()
            if options.sink_at and not drive_state.get("sunk") and time.monotonic() - started >= options.sink_at and last_position:
                px, py, pz, yaw = last_position
                link.push(RING_TO_HALO, MSG_TELEPORT, struct.pack("<4f", px, py, pz - 100.0, yaw))
                print(f"fake_skyrim: sink: Chief to 100 units under ({px:.1f} {py:.1f} {pz:.1f})", flush=True)
                drive_state["sunk"] = True
            if options.skyrim_moves:
                # the player walks north at 300 units a second after 2 s, on the terrain
                t = time.monotonic() - started
                walked = max(0.0, t - 2.0) * options.speed
                px, py = options.x, options.y + walked
                pz = options.z + terrain_height(px - options.x, py - options.y)
                drive_state["skyrim_frame"] = drive_state.get("skyrim_frame", 0) + 1
                pitch = math.radians(options.pitch)
                fy, fz = math.cos(pitch), math.sin(pitch)
                link.slot_write(SLOT_SKYRIM_PLAYER, struct.pack("<II3fff3f3f3f2I",
                    drive_state["skyrim_frame"], 0x1 | 0x2, px, py, pz, 0.0, -pitch,
                    px, py, pz + 120.0, 0.0, fy, fz, 0.0, options.speed if t > 2 else 0.0, 0.0, 0, 0))
                if display:
                    # the camera Skyrim renders with (lockstep): the same eye and view
                    LAST_CAMERA[0] += 1
                    link.slot_write(OFF_CAMERA, struct.pack("<II3f3f3f3f2I", LAST_CAMERA[0], 0,
                        px, py, pz + 120.0, 0.0, fy, fz, 0.0, -fz, fy, math.radians(60.0), 5.0, 300000.0, 0, 0))
                drive_state["skyrim_pos"] = (px, py, pz)
            if options.fire_at and not options.drive:
                t = time.monotonic() - started
                firing = options.fire_at <= t < options.fire_at + options.fire_for
                frame += 1
                link.slot_write(SLOT_INPUT, struct.pack(INPUT_FORMAT, frame, 0, (1 << 2) if firing else 0, 1,
                                                        *presses, 0.0, 0.0, 0.0, 0.0))
            if options.grenade_at and not options.drive:
                t = time.monotonic() - started
                frame += 1
                if t >= options.grenade_at and not drive_state.get("thrown"):
                    presses[5] = (presses[5] + 1) & 0xFF  # CR_ACTION_GRENADE
                    drive_state["thrown"] = True
                    print("fake_skyrim: throw a grenade", flush=True)
                link.slot_write(SLOT_INPUT, struct.pack(INPUT_FORMAT, frame, 0, 0, 1,
                                                        *presses, 0.0, 0.0, 0.0, 0.0))
            if options.zoom_at and not options.drive:
                t = time.monotonic() - started
                frame += 1
                if t >= options.zoom_at and not drive_state.get("switched"):
                    presses[8] = (presses[8] + 1) & 0xFF  # CR_ACTION_SWITCH_WEAPON
                    drive_state["switched"] = True
                    print("fake_skyrim: switch weapon", flush=True)
                zooming = t >= options.zoom_at + 2.0
                if zooming and not drive_state.get("zoomed"):
                    drive_state["zoomed"] = True
                    print("fake_skyrim: zoom held", flush=True)
                link.slot_write(SLOT_INPUT, struct.pack(INPUT_FORMAT, frame, 0, (1 << 3) if zooming else 0, 1,
                                                        *presses, 0.0, 0.0, 0.0, 0.0))
            if options.flashlight_at and not options.drive:
                t = time.monotonic() - started
                frame += 1
                for at, name in ((options.flashlight_at, "on"), (options.flashlight_at + 4.0, "off")):
                    if t >= at and not drive_state.get("flashlight " + name):
                        presses[10] = (presses[10] + 1) & 0xFF  # CR_ACTION_FLASHLIGHT
                        drive_state["flashlight " + name] = True
                        print(f"fake_skyrim: flashlight {name}", flush=True)
                link.slot_write(SLOT_INPUT, struct.pack(INPUT_FORMAT, frame, 0, 0, 1,
                                                        *presses, 0.0, 0.0, 0.0, 0.0))
            t = time.monotonic() - started
            if options.actor:
                dead = options.actor_dies_at and t >= options.actor_dies_at
                if dead and not drive_state.get("actor dead"):
                    drive_state["actor dead"] = True
                    print("fake_skyrim: the actor dies", flush=True)
                listed = not (dead and t >= options.actor_dies_at + 3.0)
                attacking = options.actor_attack_at and options.actor_attack_at <= t < options.actor_attack_at + 3.0
                flags = (0 if options.actor_friendly else 0x1) | (0x2 if dead else 0) | (0x8 if attacking else 0) | (0x10 if options.actor_sneaks else 0)
                walked = options.actor_walk * t % 1600.0 if options.actor_walk and not dead else 0.0
                walked = walked if walked < 800.0 else 1600.0 - walked  # 0..800 and back
                link.slot_write(OFF_ACTORS, actors_payload(frame, (0x0001A2B3, flags, options.x - 400.0 + walked if options.actor_walk else options.x,
                    options.y + options.actor, options.z, math.pi) if listed else None, options.actor_shape))
            if options.hurt_at and t >= options.hurt_at + drive_state.get("hurts", 0) and drive_state.get("hurts", 0) < options.hurt_count:
                drive_state["hurts"] = drive_state.get("hurts", 0) + 1
                link.push(RING_TO_HALO, MSG_PLAYER_HURT, struct.pack("<fII3f2I", options.hurt_amount, 1, 0x0001A2B3,
                    options.x, options.y + 300.0, options.z + 60.0, 0, 0))
                print(f"fake_skyrim: player hurt #{drive_state['hurts']} ({options.hurt_amount:.2f} of Chief)", flush=True)
            if options.light >= 0.0 and time.monotonic() >= drive_state.get("next_light", 0.0):
                drive_state["next_light"] = time.monotonic() + 0.1
                level = options.light_to if options.light_to >= 0.0 and t >= options.light_at else options.light
                a, k = 0.3 * level, 1.0 * level
                torch = [float(v) for v in options.torch.split(",")] if options.torch else []
                seen = options.sun_visible_to if options.sun_visible_to >= 0.0 and t >= options.sun_visible_at else options.sun_visible
                link.push(RING_TO_HALO, MSG_LIGHTING, struct.pack("<3f3f3f3fI", a, a, a, 0.0, 0.0, 0.0, *(k * float(v) for v in options.sun_color.split(",")),
                                                                  0.3, 0.4, -0.866, 1 if options.torch else 0)
                          + (struct.pack("<3ff3f", options.x + torch[0], options.y + torch[1], options.z + torch[2], *torch[3:7])
                             if options.torch else b"") + bytes(4 * 28 - (28 if options.torch else 0))
                          + struct.pack("<Iff", 1 if seen >= 0.0 else 0, max(seen, 0.0), options.sky_visible if seen >= 0.0 else 1.0))
            if options.key_names and not drive_state.get("named"):
                # the plugin's CR_MSG_KEY_NAMES: per CR_ACTION_*, 16 bytes each
                names = options.key_names.split(",") + [""] * 12
                link.push(RING_TO_HALO, MSG_KEY_NAMES, b"".join(n.encode()[:15].ljust(16, b"\0") for n in names[:12]))
                drive_state["named"] = True
                print(f"fake_skyrim: key names {names[:12]}", flush=True)
            if options.restore_at and t >= options.restore_at and not drive_state.get("restored") and drive_state.get("kit"):
                kit = drive_state["kit"]
                held = [w for w in kit["weapons"] if w["tag"]][::-1]
                weapons = [dict(w, total=[n // 2 for n in w["total"]], loaded=[n // 2 for n in w["loaded"]]) for w in held]
                weapons += [{"tag": "", "total": [0, 0], "loaded": [0, 0], "age": 0.0}] * (4 - len(weapons))
                restore = dict(kit, generation=7, flags=0, current=len(held) - 1, grenade=1, grenades=[3, 2, 0, 0],
                               body=0.5, shield=0.25, flashlight=0.4, weapons=weapons)
                link.push(RING_TO_HALO, MSG_CHIEF_RESTORE, kit_pack(restore))
                drive_state["restored"] = True
                print(f"fake_skyrim: restore {kit_text(restore)}", flush=True)
            if options.heal_at and t >= options.heal_at and not drive_state.get("healed"):
                link.push(RING_TO_HALO, MSG_CHIEF_HEAL, struct.pack("<fI", options.heal_amount, 0x0003EADE))
                drive_state["healed"] = True
                print(f"fake_skyrim: heal {options.heal_amount:.2f} of Chief's vitality", flush=True)
            if options.loadout_at and t >= options.loadout_at and not drive_state.get("loaded_out"):
                names = [n.strip() for n in options.loadout.split(",") if n.strip()][:4]
                weapons = [{"tag": n, "total": [-1, -1], "loaded": [-1, -1], "age": 0.0} for n in names]
                weapons += [{"tag": "", "total": [0, 0], "loaded": [0, 0], "age": 0.0}] * (4 - len(weapons))
                kit = {"generation": 9, "flags": 0, "current": 0, "grenade": 0, "grenades": [2, 3, 0, 0],
                       "body": 1.0, "shield": 1.0, "flashlight": 1.0, "weapons": weapons}
                link.push(RING_TO_HALO, MSG_CHIEF_RESTORE, kit_pack(kit))
                drive_state["loaded_out"] = True
                print(f"fake_skyrim: starting loadout {names}", flush=True)
            if options.restore_default_at and t >= options.restore_default_at and not drive_state.get("defaulted"):
                link.push(RING_TO_HALO, MSG_CHIEF_RESTORE, kit_pack({"generation": 8, "flags": 1, "current": -1, "grenade": -1,
                    "grenades": [0, 0, 0, 0], "body": 0.0, "shield": 0.0, "flashlight": 0.0,
                    "weapons": [{"tag": "", "total": [0, 0], "loaded": [0, 0], "age": 0.0}] * 4}))
                drive_state["defaulted"] = True
                print("fake_skyrim: restore: the starting loadout", flush=True)
            if options.give_at and t >= options.give_at + drive_state.get("gives", 0) and drive_state.get("gives", 0) < options.give_count:
                drive_state["gives"] = drive_state.get("gives", 0) + 1
                names = [n.strip() for n in options.give_name.split(",") if n.strip()]
                name = names[(drive_state["gives"] - 1) % len(names)] if names else ""
                link.push(RING_TO_HALO, MSG_GIVE_WEAPON, struct.pack("<iI64s", -1, 0, name.encode()[:63]))
                print(f"fake_skyrim: give weapon #{drive_state['gives']} {name!r}", flush=True)
            if options.cache and t >= options.cache_at and t >= drive_state.get("cache_next", 0.0):
                drive_state["cache_next"] = t + 2.0
                # cr_msg_cache_place: id, generation, position, yaw, weapon
                link.push(RING_TO_HALO, MSG_CACHE_PLACE, struct.pack("<II3ff64s", 0xCAC1, generation,
                    options.x, options.y + options.cache_north, options.z + 120.0, math.pi / 2, options.cache.encode()[:63]))
                if not drive_state.get("cache_said"):
                    drive_state["cache_said"] = True
                    print(f"fake_skyrim: cache CAC1: {options.cache} {options.cache_north:.0f} units north, world {generation}", flush=True)
            if options.action_at and not options.drive:
                t = time.monotonic() - started
                frame += 1
                holding = options.action_at <= t < options.action_at + 2.0
                link.slot_write(SLOT_INPUT, struct.pack(INPUT_FORMAT, frame, 0, (1 << 7) if holding else 0, 1,
                                                        *presses, 0.0, 0.0, 0.0, 0.0))
            if options.list_weapons_at and t >= options.list_weapons_at and not drive_state.get("listed"):
                drive_state["listed"] = True
                link.push(RING_TO_HALO, MSG_GIVE_WEAPON, struct.pack("<iI64s", -1, GIVE_LIST, b""))
                print("fake_skyrim: chiefrim weapons", flush=True)
            if options.shapes and not drive_state.get("shapes"):
                drive_state["shapes"] = True
                link.push(RING_TO_HALO, MSG_DEBUG, struct.pack("<II", DEBUG_HITBOXES, 0))
                print("fake_skyrim: chiefrim shapes on", flush=True)
            if display:
                display[2] += 1
                link.slot_write(OFF_DISPLAY, struct.pack("<4I", display[0], display[1], 0x1, display[2]))
                if options.grab and time.monotonic() - last_grab >= options.grab_every:
                    if grab_frame(link, options.grab, grabs):
                        grabs += 1
                    last_grab = time.monotonic()
            if link.u32(HALO_STATE) == SIDE_CLOSING:
                print("fake_skyrim: Halo is closing")
                break
            now = time.monotonic()
            if now - last_print >= (PRINT_EVERY if options.drive else 0.5):
                seq, payload = link.slot_read(SLOT_PLAYER, 88)
                if payload and seq != last_seq:
                    v = struct.unpack("<II3f2fI3f3f3f3f2I", payload)  # ..., time_us, reserved
                    tick, pose, px, py, pz, yaw, pitch, on_ground = v[0:8]
                    if on_ground:
                        last_position = (px, py, pz, yaw)
                    ex, ey, ez = v[8:11]
                    fov, body, shield = v[17], v[18], v[19]
                    rate = "" if last_tick is None else f" ({(tick - last_tick) / (now - last_print):.0f} frames/s)"
                    print(f"player: pos ({px:9.1f} {py:9.1f} {pz:8.1f}) heading {math.degrees(yaw):6.1f}"
                          f" pitch {math.degrees(pitch):5.1f} {POSES.get(pose, pose):9} ground {on_ground}"
                          f" eye z {ez:8.1f} fov {math.degrees(fov):5.1f} body {body:.2f} shield {shield:.2f}{rate}",
                          flush=True)
                    last_seq, last_tick = seq, tick
                last_print = now
            time.sleep(0.005)
    except KeyboardInterrupt:
        pass
    link.set_u32(SKYRIM_STATE, SIDE_CLOSING)
    return 0


if __name__ == "__main__":
    sys.exit(main())
