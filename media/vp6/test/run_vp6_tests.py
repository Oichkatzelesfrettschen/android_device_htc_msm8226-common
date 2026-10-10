#!/usr/bin/env python3
"""Host test for the vendored VP6 decoder.

Fetches the public sample FLV files listed in samples.txt (sha256-checked),
decodes their VP6 and VP6A tracks through the a11 FLV parser and the vendored
decoder built with ASan and UBSan, and requires the output to equal ffmpeg's
own yuv420p decode byte for byte.

Usage: run_vp6_tests.py WORKDIR
"""
import hashlib
import os
import subprocess
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
CONT = os.path.abspath(os.path.join(ROOT, "..", "containers"))


def run(cmd, **kw):
    return subprocess.run(cmd, check=True, capture_output=True, text=True, **kw)


def main():
    work = sys.argv[1]
    os.makedirs(work, exist_ok=True)
    inc = ["-I" + ROOT + "/compat", "-I" + ROOT + "/third_party/ffmpeg",
           "-I" + ROOT + "/third_party/ffmpeg/libavcodec"]
    srcs = ["libavcodec/" + n + ".c" for n in (
        "vp6", "vp56", "vp56data", "vp6dsp", "vpx_rac", "vp3dsp", "huffman", "vlc", "hpeldsp",
        "h264chroma", "videodsp", "mathtables")] + ["libavutil/reverse.c"]
    objs = []
    for s in [os.path.join(ROOT, "third_party/ffmpeg", x) for x in srcs] + [
            os.path.join(ROOT, "compat/compat.c"), os.path.join(ROOT, "a11vp6.c")]:
        o = os.path.join(work, os.path.basename(s) + ".o")
        run(["clang", "-std=gnu11", "-O2", "-g", "-fsanitize=address,undefined", "-fno-strict-aliasing"]
            + inc + ["-c", s, "-o", o])
        objs.append(o)
    exe = os.path.join(work, "vp6_host_test")
    run(["clang++", "-std=c++17", "-O2", "-g", "-fsanitize=address,undefined", "-I" + CONT, "-I" + ROOT,
         "-o", exe, os.path.join(HERE, "vp6_host_test.cpp")]
        + [os.path.join(CONT, n) for n in ("FlvParser.cpp", "AviParser.cpp", "Codecs.cpp", "TrackReader.cpp")]
        + objs)
    for line in open(os.path.join(HERE, "samples.txt")):
        if line.startswith("#") or not line.strip():
            continue
        name, url, digest = line.split()
        path = os.path.join(work, name)
        if not os.path.exists(path):
            urllib.request.urlretrieve(url, path)
        got = hashlib.sha256(open(path, "rb").read()).hexdigest()
        if got != digest:
            print("FAIL: sha256 of", name, got)
            sys.exit(1)
        ours = os.path.join(work, name + ".ours.yuv")
        ref = os.path.join(work, name + ".ref.yuv")
        r = run([exe, path, ours]).stdout
        run(["ffmpeg", "-v", "error", "-y", "-i", path, "-an", "-f", "rawvideo", "-pix_fmt", "yuv420p", ref])
        if open(ours, "rb").read() != open(ref, "rb").read():
            print("FAIL: decode of", name, "differs from ffmpeg")
            sys.exit(1)
        print("ok", name, r.strip())
    print("all ok")


if __name__ == "__main__":
    main()
