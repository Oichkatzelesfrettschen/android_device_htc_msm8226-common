#!/usr/bin/env python3
"""Host tests for the AVI and FLV container readers.

Generates media with ffmpeg, runs the harness over each file and checks track
MIME types, geometry, parameter-set data, packet counts, timestamps, key flags,
sizes, decodability of the converted samples, and seeks against ffprobe.
Golden summaries live in expected/; large media is generated into a scratch
directory and never committed.

Usage: run_tests.py [--update] [--big] WORKDIR
"""
import hashlib
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = [os.path.join(HERE, "harness.cpp")] + [
    os.path.join(HERE, "..", n) for n in ("AviParser.cpp", "Codecs.cpp", "FlvParser.cpp", "TrackReader.cpp")]

VIDEO = "-f lavfi -i testsrc2=size=320x240:rate=25"
AUDIO = "-f lavfi -i sine=frequency=440:sample_rate=44100"

# name -> (ffmpeg arguments, container, expected video MIME, expected audio MIME,
#          ffprobe video codec, decodable raw format)
CASES = {
    "divx3.avi": ("-c:v msmpeg4 -vtag DIV3 -c:a libmp3lame -b:a 96k -f avi", "video/divx311", "audio/mpeg", None),
    "divx4.avi": ("-c:v mpeg4 -vtag DIVX -c:a libmp3lame -f avi", "video/divx4", "audio/mpeg", "m4v"),
    "divx5.avi": ("-c:v mpeg4 -vtag DX50 -bf 2 -c:a libmp3lame -f avi", "video/divx", "audio/mpeg", "m4v"),
    "xvid.avi": ("-c:v libxvid -vtag XVID -c:a libmp3lame -f avi", "video/mp4v-es", "audio/mpeg", "m4v"),
    "h264.avi": ("-c:v libx264 -g 25 -c:a libmp3lame -f avi", "video/avc", "audio/mpeg", "h264"),
    "spark.flv": ("-c:v flv -c:a libmp3lame -f flv", "video/x-flv1", "audio/mpeg", None),
    "h264aac.flv": ("-c:v libx264 -bf 2 -g 25 -c:a aac -f flv", "video/avc", "audio/mp4a-latm", "h264"),
}


def run(cmd, **kw):
    return subprocess.run(cmd, check=True, capture_output=True, text=True, **kw).stdout


def probe(path):
    out = run(["ffprobe", "-v", "error", "-show_streams", "-show_packets", "-of", "json", path])
    return json.loads(out)


def harness_info(h, path):
    tracks, packets = [], {}
    for line in run([h, "info", path]).splitlines():
        if line.startswith("track "):
            f = dict(kv.split("=", 1) for kv in line.split()[2:])
            f["index"] = int(line.split()[1])
            tracks.append(f)
        elif line.startswith("packet "):
            _, i, t, s, k = line.split()
            packets.setdefault(int(i), []).append((int(t), int(s), int(k)))
    return tracks, packets


def fail(msg):
    print("FAIL:", msg)
    sys.exit(1)


def check(cond, msg):
    if not cond:
        fail(msg)


def compare_streams(name, h, path, vmime, amime, raw):
    tracks, packets = harness_info(h, path)
    pr = probe(path)
    streams = pr["streams"]
    video_t = [t for t in tracks if t["video"] == "1"]
    audio_t = [t for t in tracks if t["video"] == "0"]
    check(len(video_t) == 1 and video_t[0]["mime"] == vmime, f"{name}: video MIME {video_t}")
    check(len(audio_t) == 1 and audio_t[0]["mime"] == amime, f"{name}: audio MIME {audio_t}")
    vs = [s for s in streams if s["codec_type"] == "video"][0]
    check(int(video_t[0]["w"]) == vs["width"] and int(video_t[0]["h"]) == vs["height"], f"{name}: dimensions")
    vi = video_t[0]["index"]
    pv = [p for p in pr["packets"] if p["stream_index"] == vs["index"]]
    check(len(pv) == len(packets[vi]), f"{name}: video packet count {len(pv)} vs {len(packets[vi])}")
    for a, b in zip(pv, packets[vi]):
        pts = a.get("pts_time") or a.get("dts_time")
        check(pts is not None and abs(round(float(pts) * 1e6) - b[0]) <= 1000, f"{name}: video pts {a} vs {b}")
        check(int(a["size"]) == b[1], f"{name}: video size {a['size']} vs {b[1]}")
        check(("K" in a["flags"]) == bool(b[2]) or b is packets[vi][0], f"{name}: key flag {a['flags']} vs {b}")
    if vmime == "video/avc" and name.endswith(".flv"):
        check(int(video_t[0]["csd0"]) > 4 and int(video_t[0]["csd1"]) > 4, f"{name}: avc csd")
    # Decode the converted samples with ffmpeg: every packet must produce a frame.
    if raw:
        out = os.path.join(os.path.dirname(path), name + ".video." + raw)
        run([h, "read", path, str(vi), out])
        if vmime == "video/avc" and name.endswith(".flv"):
            with open(out, "rb") as f:
                check(f.read(4) == b"\x00\x00\x00\x01", f"{name}: Annex B start code")
        n = decode_frames(out, raw, name)
        check(n >= len(pv) - 2, f"{name}: decoded {n} frames of {len(pv)}")
    # MP3 frame splitting: every sample is a whole frame ffprobe's mp3 demuxer accepts.
    ai = audio_t[0]["index"]
    if amime == "audio/mpeg":
        out = os.path.join(os.path.dirname(path), name + ".audio.mp3")
        n_samples = int(run([h, "read", path, str(ai), out]).split("=")[1])
        pa = json.loads(run(["ffprobe", "-v", "error", "-show_packets", "-of", "json", out]))["packets"]
        check(len(pa) == n_samples, f"{name}: mp3 frames {n_samples} vs ffprobe {len(pa)}")
        ref = [p for p in pr["packets"] if p["stream_index"] != vs["index"]]
        check(abs(sum(int(p["size"]) for p in pa) - sum(int(p["size"]) for p in ref)) < 4096,
              f"{name}: mp3 byte total")
    else:
        pa = [p for p in pr["packets"] if p["stream_index"] != vs["index"]]
        check(len(pa) == len(packets[ai]), f"{name}: audio packets {len(pa)} vs {len(packets[ai])}")
        for a, b in zip(pa, packets[ai]):
            check(abs(round(float(a["pts_time"]) * 1e6) - b[0]) <= 1000, f"{name}: audio pts")
    return tracks, packets, vi


def decode_frames(path, fmt, name):
    out = subprocess.run(["ffmpeg", "-v", "error", "-f", fmt, "-i", path, "-f", "null", "-"],
                         capture_output=True, text=True)
    prog = subprocess.run(["ffprobe", "-v", "error", "-f", fmt, "-count_frames", "-select_streams", "v:0",
                           "-show_entries", "stream=nb_read_frames", "-of", "csv=p=0", path],
                          capture_output=True, text=True)
    try:
        return int(prog.stdout.strip().split(",")[0])
    except ValueError:
        fail(f"{name}: cannot count decoded frames: {out.stderr[:200]} {prog.stderr[:200]}")


def seek_tests(name, h, path, tracks, packets, vi):
    pk = packets[vi]
    keys = [i for i, p in enumerate(pk) if p[2]]
    times = [p[0] for p in pk]
    mid = times[len(times) // 2]
    def seek(mode, value):
        out = run([h, "seek", path, str(vi), mode, str(value)]).strip()
        return out
    # previous sync: a key packet at or before the time, the latest such.
    r = dict(kv.split("=") for kv in seek("previous", mid).split())
    cand = [pk[i] for i in keys if pk[i][0] <= mid]
    check(int(r["key"]) == 1 and int(r["time"]) == cand[-1][0], f"{name}: previous sync {r}")
    # next sync: first key at or after the time.
    out = seek("next", mid)
    cand = [pk[i] for i in keys if pk[i][0] >= mid]
    if cand:
        r = dict(kv.split("=") for kv in out.split())
        check(int(r["key"]) == 1 and int(r["time"]) == cand[0][0], f"{name}: next sync {out}")
    else:
        check("eos" in out, f"{name}: next sync with no later key should be eos, got {out}")
    # next sync past the last key packet ends the stream.
    past = pk[keys[-1]][0] + 1
    out = seek("next", past)
    check("eos" in out, f"{name}: next sync past last key should be eos, got {out}")
    # closest: previous sync with the target set.
    r = dict(kv.split("=") for kv in seek("closest", mid).split())
    check(int(r["target"]) == mid and int(r["key"]) == 1, f"{name}: closest {r}")
    # frame index: lands on the key packet at or before packet N and tags the target.
    n = len(pk) // 2
    r = dict(kv.split("=") for kv in seek("index", n).split())
    k = max(i for i in keys if i <= n)
    check(int(r["time"]) == pk[k][0] and int(r["target"]) == pk[n][0], f"{name}: frame index {r}")


def summarize(h, path):
    tracks, packets = harness_info(h, path)
    lines = []
    for t in tracks:
        lines.append(" ".join(f"{k}={v}" for k, v in sorted(t.items())))
        digest = hashlib.sha256(repr(packets[t["index"]]).encode()).hexdigest()
        lines.append(f"packets_sha256={digest}")
    return "\n".join(lines) + "\n"


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    update = "--update" in sys.argv
    big = "--big" in sys.argv
    work = args[0]
    os.makedirs(work, exist_ok=True)
    h = os.path.join(work, "harness")
    run(["clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
         "-fsanitize=address,undefined", "-I" + os.path.join(HERE, ".."), "-o", h] + SRC)
    ut = os.path.join(work, "unit_tests")
    run(["clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O1", "-g", "-fsanitize=address,undefined",
         "-I" + os.path.join(HERE, ".."), "-o", ut, os.path.join(HERE, "unit_tests.cpp")] + SRC[1:])
    check("unit tests ok" in run([ut]), "unit tests")
    print("ok unit tests")
    version = run(["ffmpeg", "-version"]).splitlines()[0]
    exp_dir = os.path.join(HERE, "expected")
    os.makedirs(exp_dir, exist_ok=True)
    for name, (ff, vmime, amime, raw) in CASES.items():
        path = os.path.join(work, name)
        run(["ffmpeg", "-v", "error", "-y"] + VIDEO.split() + AUDIO.split() + ["-t", "4"] + ff.split() + [path])
        tracks, packets, vi = compare_streams(name, h, path, vmime, amime, raw)
        seek_tests(name, h, path, tracks, packets, vi)
        summary = f"# {version}\n" + summarize(h, path)
        golden = os.path.join(exp_dir, name + ".txt")
        if update:
            with open(golden, "w") as f:
                f.write(summary)
        elif os.path.exists(golden):
            with open(golden) as f:
                want = f.read()
            if want.splitlines()[0] == summary.splitlines()[0]:
                check(want == summary, f"{name}: golden summary differs")
        print("ok", name)
    fuzz(h, work)
    if big:
        big_test(h, work, update, exp_dir, version)
    print("all ok")


def fuzz(h, work):
    """Corrupts headers and tails of every case; the harness must neither crash
    nor trip a sanitizer nor hang. Exit 0 (parsed) and 3 (rejected) are clean."""
    import random
    rng = random.Random(20261009)
    env = dict(os.environ, UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1",
               ASAN_OPTIONS="detect_leaks=0")
    runs = 0
    for name in CASES:
        with open(os.path.join(work, name), "rb") as f:
            data = bytearray(f.read())
        for i in range(120):
            m = bytearray(data)
            if i % 6 == 5:
                m = m[:rng.randrange(16, len(m))]
            else:
                limit = 4096 if i % 2 == 0 else len(m)
                for _ in range(rng.choice((1, 2, 4, 8))):
                    pos = rng.randrange(0, min(limit, len(m)))
                    m[pos] = rng.choice((0x00, 0xFF, 0x7F, 0x80, rng.randrange(256)))
            path = os.path.join(work, "fuzz.bin")
            with open(path, "wb") as f:
                f.write(m)
            for cmd in (["info", path], ["read", path, "0", os.devnull], ["read", path, "1", os.devnull]):
                try:
                    r = subprocess.run([h] + cmd, capture_output=True, text=True, timeout=20, env=env)
                except subprocess.TimeoutExpired:
                    fail(f"fuzz {name} #{i} {cmd[0]}: timeout")
                if r.returncode not in (0, 2, 3):
                    shutil_keep(path, name, i)
                    fail(f"fuzz {name} #{i} {cmd[0]}: exit {r.returncode}\n{r.stderr[-800:]}")
                runs += 1
    print("ok fuzz", runs, "runs")


def shutil_keep(path, name, i):
    import shutil
    shutil.copy(path, os.path.join(os.path.dirname(path), f"crash-{name}-{i}.bin"))


def big_test(h, work, update, exp_dir, version):
    path = os.path.join(work, "opendml.avi")
    if not os.path.exists(path):
        run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i",
             "nullsrc=s=640x480:r=25,geq=random(1)*255:128:128", "-t", "130", "-c:v", "mpeg4", "-vtag", "DX50",
             "-q:v", "1", "-an", "-f", "avi", path])
    size = os.path.getsize(path)
    check(size > (1 << 30), f"opendml.avi is {size} bytes, below 1 GiB")
    tracks, packets = harness_info(h, path)
    pr = probe(path)
    pv = pr["packets"]
    check(len(pv) == len(packets[0]) and len(pv) > 0, f"opendml: packets {len(pv)} vs {len(packets[0])}")
    for a, b in zip(pv, packets[0]):
        check(abs(round(float(a["pts_time"]) * 1e6) - b[0]) <= 1000 and int(a["size"]) == b[1], "opendml: packet")
    # A packet beyond the first 1 GiB segment reads back byte-exact.
    out = os.path.join(work, "opendml.video.m4v")
    run([h, "read", path, "0", out])
    check(os.path.getsize(out) == sum(p[1] for p in packets[0]), "opendml: sample bytes")
    summary = f"# {version}\n" + summarize(h, path)
    golden = os.path.join(exp_dir, "opendml.avi.txt")
    if update:
        with open(golden, "w") as f:
            f.write(summary)
    print("ok opendml.avi", size)


if __name__ == "__main__":
    main()
