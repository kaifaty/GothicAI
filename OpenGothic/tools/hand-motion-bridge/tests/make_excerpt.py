"""Cut the self-test excerpt of the hand-motion bridge out of two Joy-Con probe runs (2026-11-14).

    python3 make_excerpt.py <run 20261114-201521>/samples.csv <run 20261114-201802>/samples.csv excerpt.csv

Part 1: run 201521, both Joy-Con lying on the table, 20.0-23.5 s after the first cue. Then 2 s without samples.
Part 2: run 201802, the right Joy-Con only, 8.5-25.5 s: on the table, picked up, held, three swings, held.
Rows follow the bridge's record format (README); arrival is the SDL event time of the packet's newest sample,
moved so that the excerpt starts at 1 s.
"""
import csv
import sys

PART1 = (20.0, 23.5)
PART2 = (8.5, 25.5)
GAP_US = 2_000_000
START_US = 1_000_000


def packets(path, window, hands):
    rows = list(csv.DictReader(open(path, newline="")))
    names = {r["pad"]: r["text"].split("|")[0] for r in rows if r["kind"] == "added"}
    cue0 = next(int(r["event_ns"]) for r in rows if r["kind"] == "cue")
    lo, hi = cue0 + int(window[0] * 1e9), cue0 + int(window[1] * 1e9)
    samples = {}  # (pad, sensor_ns) -> [event_ns, gyro, accel]
    order = []
    for r in rows:
        if r["kind"] != "s":
            continue
        hand = "L" if names[r["pad"]].endswith("(L)") else "R"
        if hand not in hands:
            continue
        key = (hand, int(r["sensor_ns"]))
        if key not in samples:
            samples[key] = [int(r["event_ns"]), None, None]
            order.append(key)
        samples[key][1 if r["sensor"] == "1" else 2] = (r["x"], r["y"], r["z"])
    out = []  # (hand, [(event_ns, sensor_ns, gyro, accel)])
    open_packet = {}
    for key in order:
        ev, gyro, accel = samples[key]
        if gyro is None or accel is None:
            continue
        hand = key[0]
        cur = open_packet.get(hand)
        if cur is not None and ev - cur[-1][0] < 1_000_000:
            cur.append((ev, key[1], gyro, accel))
        else:
            cur = [(ev, key[1], gyro, accel)]
            open_packet[hand] = cur
            out.append((hand, cur))
    return [(h, p) for h, p in out if lo <= p[-1][0] < hi], lo, names


def write(f, part, origin_ns, shift_us):
    end = 0
    for hand, p in part:
        arrival = (p[-1][0] - origin_ns) // 1000 + shift_us
        for _, sensor_ns, g, a in p:
            sensor_us = sensor_ns // 1000
            gx, gy, gz = (float(v) for v in g)
            ax, ay, az = (float(v) for v in a)
            f.write(f"{hand},{arrival},{sensor_us},{gx:.4f},{gy:.4f},{gz:.4f},{ax:.3f},{ay:.3f},{az:.3f},0\n")
        end = max(end, arrival)
    return end


def main(run1, run2, out):
    p1, lo1, names1 = packets(run1, PART1, "LR")
    p2, lo2, _ = packets(run2, PART2, "R")
    with open(out, "w", newline="") as f:
        f.write("# hand-motion-bridge record v1\n")
        f.write("hand,arrival_us,sensor_us,gx,gy,gz,ax,ay,az,buttons\n")
        for hand in "LR":
            name = next(n for n in names1.values() if n.endswith(f"({hand})"))
            f.write(f"# device {hand} {name}\n")
        end = write(f, p1, lo1, START_US)
        write(f, p2, lo2, end + GAP_US)


if __name__ == "__main__":
    main(*sys.argv[1:4])
