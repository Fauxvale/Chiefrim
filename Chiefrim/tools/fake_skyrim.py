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
VERSION = 2
RING_BYTES = 256 * 1024
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
ACTION_JUMP, ACTION_CROUCH = 0, 1
INPUT_FORMAT = "<IIII16Bffdd"  # cr_input (protocol v2)

# (start s, end s, what): the --drive script, after a 2 s settle
DRIVE = [(2, 4, "forward"), (4, 6, "strafe right"), (6, 8, "turn right"),
         (8, 8.1, "jump"), (10, 12, "crouch"), (12, 13, "forward"),
         (13, 15, "stall"), (15, 16, "stop")]


def drive_input(t, frame, presses, state):
    """the cr_input for t seconds into the run"""
    forward = strafe = 0.0
    held = 0
    phase = next((what for start, end, what in DRIVE if start <= t < end), "stop")
    if phase == "forward":
        forward = 1.0
    elif phase == "strafe right":
        strafe = 1.0
    elif phase == "turn right":
        state["yaw"] += 0.02  # radians per tick of this loop (~200/s): ~4 rad/s
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
    parser.add_argument("--silence-at", type=float, default=0.0,
                        help="seconds in: stop the heartbeat, as a Skyrim that hangs (0: never)")
    parser.add_argument("--silence-for", type=float, default=5.0)
    parser.add_argument("--drive", action="store_true",
                        help="script Chief through the input slot: forward, strafe right, turn right, jump, crouch")
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
    # cr_world_context: world_id, is_interior, origin, floor_z, generation, reserved[3]
    link.slot_write(SLOT_WORLD, struct.pack("<II3ffI3I",
        TAMRIEL, 0, options.x, options.y, options.z, options.z, 1, 0, 0, 0))
    link.push(RING_TO_HALO, MSG_TELEPORT,
        struct.pack("<4f", options.x, options.y, options.z, math.radians(options.heading)))

    last_print = 0.0
    last_seq = 0
    last_tick = None
    started = time.monotonic()
    silenced = False
    frame = 0
    presses = [0] * 16
    drive_state = {"yaw": 0.0}
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
                stalled = any(start <= t < end and what == "stall" for start, end, what in DRIVE)
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
            if link.u32(HALO_STATE) == SIDE_CLOSING:
                print("fake_skyrim: Halo is closing")
                break
            now = time.monotonic()
            if now - last_print >= (0.25 if options.drive else 0.5):
                seq, payload = link.slot_read(SLOT_PLAYER, 88)
                if payload and seq != last_seq:
                    v = struct.unpack("<II3f2fI3f3f3f3f2I", payload)
                    tick, pose, px, py, pz, yaw, pitch, on_ground = v[0:8]
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
