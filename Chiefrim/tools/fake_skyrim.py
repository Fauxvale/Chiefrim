#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A stand-in for the Skyrim side of the link (docs/DESIGN.md §12, Phase 0).

It maps /dev/shm/chiefrim_v1 as the SKSE plugin will, says hello, sends a
world context (origin and floor at a Skyrim position) and a Teleport, keeps
a heartbeat, and prints the player state Halo publishes, so the Halo side
can be tested without Skyrim.

Usage: tools/fake_skyrim.py [--seconds N] [--x X --y Y --z Z --heading DEG]
  The default position is outside Whiterun's gate in Tamriel.

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
VERSION = 4
RING_BYTES = 4 * 1024 * 1024
TOTAL_SIZE = 272 + 2 * (128 + RING_BYTES)

# offsets (chiefrim_protocol.h)
SKYRIM_PID, HALO_PID = 16, 20
SKYRIM_STATE, HALO_STATE = 24, 28
SKYRIM_HEARTBEAT, HALO_HEARTBEAT = 32, 36
SLOT_WORLD, SLOT_INPUT, SLOT_PLAYER = 64, 112, 176
RING_TO_HALO, RING_TO_SKYRIM = 272, 272 + 128 + RING_BYTES

SIDE_READY, SIDE_CLOSING = 2, 3
MSG_WRAP, MSG_HELLO, MSG_TELEPORT, MSG_LOG = 0, 1, 2, 3
POSES = {0: "standing", 1: "crouching", 2: "airborne", 3: "dead"}
MSG_COLLISION_RESET, MSG_COLLISION_TRIS = 4, 5
REGION_UNITS = 1024.0
TRIS_PER_MESSAGE = 1600


ROUGH = False


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
CR_TRIANGLE_ONE_SIDED = 0x0001
LEDGE = 0.0


def terrain_triangles(ox, oy, oz):
    """(triangle as 9 floats) list, world coordinates, wound counter-clockwise from above"""
    tris = []
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
            for q in ((0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)):
                a0, a1, a2, a3 = (corners[i] for i in q)
                tris.append(a0 + a1 + a2)
                tris.append(a0 + a2 + a3)
    if LEDGE:  # a slab like a road piece across the way north, LEDGE units up, from y 80 to 200
        corners = [(ox + (300 if k & 1 else -300), oy + (200 if k & 2 else 80), oz + (LEDGE if k & 4 else -10)) for k in range(8)]
        for q in ((0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)):
            a0, a1, a2, a3 = (corners[i] for i in q)
            tris.append(a0 + a1 + a2)
            tris.append(a0 + a2 + a3)
    # a wall south of the start, 400 high, facing north
    a, b = (ox - 2000, oy - 800, oz), (ox + 2000, oy - 800, oz)
    c, d = (ox + 2000, oy - 800, oz + 400), (ox - 2000, oy - 800, oz + 400)
    tris.append(a + b + c)
    tris.append(a + c + d)
    return tris


def send_terrain(link, epoch, ox, oy, oz, generation=1):
    regions = {}
    for i, t in enumerate(terrain_triangles(ox, oy, oz)):
        cx, cy, cz = (t[0] + t[3] + t[6]) / 3, (t[1] + t[4] + t[7]) / 3, (t[2] + t[5] + t[8]) / 3
        key = (math.floor(cx / REGION_UNITS), math.floor(cy / REGION_UNITS), math.floor(cz / REGION_UNITS))
        regions.setdefault(key, []).append((t, CR_TRIANGLE_ONE_SIDED if i < GROUND_COUNT else 0))
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


def text(raw):
    return raw.split(b"\0", 1)[0].decode(errors="replace")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--seconds", type=float, default=60)
    parser.add_argument("--x", type=float, default=19500.0)
    parser.add_argument("--y", type=float, default=-7400.0)
    parser.add_argument("--z", type=float, default=-3650.0)
    parser.add_argument("--heading", type=float, default=0.0, help="degrees, 0 = north")
    parser.add_argument("--fov", type=float, default=95.0, help="Chief's field of view, as Skyrim measures it (0: Halo's own)")
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
    parser.add_argument("--hole", action="store_true",
                        help="with --terrain: no ground under the start, so Chief falls through (a test of the catch)")
    parser.add_argument("--radius", type=float, default=0.0,
                        help="Chief's collision radius in Skyrim units, as the plugin sends it (0: Halo's own)")
    parser.add_argument("--height", type=float, default=128.0,
                        help="Chief's height in Skyrim units, as the plugin sends it (0: Halo's own)")
    options = parser.parse_args()

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
    link.slot_write(SLOT_WORLD, struct.pack("<II3ffIfff",
        TAMRIEL, 0, options.x, options.y, start_z, start_z, 1, options.fov, options.height, options.radius))
    link.push(RING_TO_HALO, MSG_TELEPORT,
        struct.pack("<4f", options.x, options.y, start_z, math.radians(options.heading)))
    if options.terrain:
        global ROUGH, HOLE, LEDGE
        ROUGH = options.rough
        HOLE = options.hole
        LEDGE = options.ledge
        send_terrain(link, 1, options.x, options.y, options.z)

    last_print = 0.0
    last_seq = 0
    last_tick = None
    started = time.monotonic()
    silenced = False
    frame = 0
    presses = [0] * 16
    drive_state = {"yaw": 0.0, "terrain": options.terrain}
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
            if (options.recenter_every > 0 and last_position and
                    time.monotonic() - last_recenter >= options.recenter_every):
                # wherever a build is: the race between a build and a new origin
                px, py, pz, yaw = last_position
                generation += 1
                epoch += 1
                link.slot_write(SLOT_WORLD, struct.pack("<II3ffIfff",
                    TAMRIEL, 0, px, py, pz, pz, generation, options.fov, options.height, options.radius))
                link.push(RING_TO_HALO, MSG_TELEPORT, struct.pack("<4f", px, py, pz + 5.0, yaw))
                print(f"fake_skyrim: recenter #{generation - 1} at ({px:.1f} {py:.1f} {pz:.1f})", flush=True)
                if options.terrain:
                    send_terrain(link, epoch, options.x, options.y, options.z, generation)
                last_recenter = time.monotonic()
            if link.u32(HALO_STATE) == SIDE_CLOSING:
                print("fake_skyrim: Halo is closing")
                break
            now = time.monotonic()
            if now - last_print >= (0.25 if options.drive else 0.5):
                seq, payload = link.slot_read(SLOT_PLAYER, 88)
                if payload and seq != last_seq:
                    v = struct.unpack("<II3f2fI3f3f3f3f2I", payload)
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
