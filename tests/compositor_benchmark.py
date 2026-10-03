#!/usr/bin/env python3
"""Compare the native CPU compositor with its small retained reference.

Run: python3 tests/compositor_benchmark.py [--sanitize] [--output FILE]
Build products are temporary. JSON records exact pixel checks plus scalar host
timings; those timings do not measure int80, GPU upload, or QEMU frame rate.
"""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    match = re.search(r"^static [^\n]*\b" + name + r"\(", source, re.M)
    if not match:
        raise ValueError("Cannot extract compositor function: " + name)
    opening = source.index("{", match.end())
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


def candidate_source():
    source = (ROOT / "user/desktop.c").read_text()
    names = ["min", "max", "clamp", "rounded_coverage",
             "compositor_mix", "compositor_radius", "compositor_span",
             "compositor_round_region", "rr", "stroke", "compositor_blur_line",
             "glass", "shadow"]
    header = """#include <stdint.h>
#include <stdbool.h>
#include "raster.h"
#define MAX_W 1920
#define MAX_H 1200
static uint32_t canvas[MAX_W*MAX_H],blur_a[500*320],blur_b[500*320];
static int sw,sh;static bool night,reduced_transparency;
"""
    wrappers = """
uint32_t *new_pixels(void){return canvas;}
void new_config(int width,int height,int dark,int reduced){sw=width;sh=height;night=dark;reduced_transparency=reduced;}
void new_draw(int kind,int x,int y,int w,int h,int r,uint32_t c,int alpha){
 if(kind==0)rr(x,y,w,h,r,c,alpha);
 if(kind==1)stroke(x,y,w,h,r,c,alpha);
 if(kind==2)shadow(x,y,w,h);
 if(kind==3)glass(x,y,w,h,r,alpha);
 if(kind==4){shadow(x,y,w,h);glass(x,y,w,h,r,alpha);rr(x+1,y+54,w-2,h-55,16,0xf8fbff,118);stroke(x,y,w,h,r,0xffffff,75);}
}
"""
    return header + "\n".join(function(source, name) for name in names) + wrappers


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    compiler = os.environ.get("CC", "gcc")
    flags = ["-std=c11", "-O1" if args.sanitize else "-O2", "-g",
             "-Wall", "-Wextra", "-Werror", "-mgeneral-regs-only", "-mno-red-zone"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    candidate = candidate_source()
    reference = ROOT / "tests/compositor_reference.c"
    harness = ROOT / "tests/compositor_benchmark.c"
    with tempfile.TemporaryDirectory(prefix="arkos-compositor-") as temp:
        generated, executable = Path(temp) / "candidate.c", Path(temp) / "check"
        generated.write_text(candidate)
        objects = []
        for source in [reference, generated, ROOT / "user/raster.c"]:
            obj = Path(temp) / (source.stem + ".o")
            subprocess.run([compiler, *flags, "-I", str(ROOT / "include"),
                            "-c", str(source), "-o", str(obj)], check=True)
            objects.append(str(obj))
        # The host timing harness uses double, while both compared compositor
        # objects retain the guest's integer-only register convention.
        host_flags = [flag for flag in flags if flag != "-mgeneral-regs-only"]
        subprocess.run([compiler, *host_flags, str(harness), *objects,
                        "-o", str(executable)], check=True)
        environment = dict(os.environ, ASAN_OPTIONS="detect_leaks=0")
        run = subprocess.run([str(executable)], check=True, text=True,
                             capture_output=True, env=environment)
    print(run.stdout, end="")
    if run.stderr:
        print(run.stderr, end="")
    checks = re.search(r"Exact pixel comparisons: (\d+) cases passed", run.stdout)
    if not checks:
        raise RuntimeError("Missing exact-pixel completion record")
    metrics = []
    for name, before, after, ratio in re.findall(
            r"^(.+?)\s+old ([\d.]+) ms new ([\d.]+) ms speedup ([\d.]+)x$",
            run.stdout, re.M):
        metrics.append({"primitive": name.strip(), "reference_ms": float(before),
                        "optimized_ms": float(after), "speedup": float(ratio)})
    if len(metrics) != 5:
        raise RuntimeError("Incomplete timing results")
    record = {
        "schema": 1, "measured_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "scope": "scalar host CPU primitives; excludes syscalls, presentation and QEMU",
        "compiler": subprocess.check_output([compiler, "--version"], text=True).splitlines()[0],
        "primitive_compiler_flags": flags, "host_timing_compiler_flags": host_flags,
        "host": platform.platform(),
        "sanitizers": ["address", "undefined"] if args.sanitize else [],
        "exact_pixel_cases": int(checks.group(1)), "exact_pixel_failures": 0,
        "timing_canvas": [1280, 800], "timing_rectangle": [122, 92, 920, 548],
        "repetitions": 40, "metrics": metrics,
        "reference_sha256": hashlib.sha256(reference.read_bytes()).hexdigest(),
        "candidate_primitives_sha256": hashlib.sha256(candidate.encode()).hexdigest(),
        "harness_sha256": hashlib.sha256(harness.read_bytes()).hexdigest(),
        "raster_sha256": hashlib.sha256((ROOT / "user/raster.c").read_bytes()).hexdigest(),
    }
    filename = "compositor-sanitizer.json" if args.sanitize else "compositor-microbenchmark.json"
    output = args.output or ROOT / "docs/verification-0.5" / filename
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(record, indent=2) + "\n")
    print("Saved", output)


if __name__ == "__main__":
    main()
