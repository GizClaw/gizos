"""Run the pinned actual BK SDK FlashDB/FAL against a file-backed NOR.

The native engine (with production GC error fix) and PAL/flash port are real; only RTOS, NOR hardware
and the separate EasyFlash migration boundary are replaced. No device E2E claim.
"""
import argparse
import contextlib
import hashlib
import json
import os
import runpy
from pathlib import Path
import shutil
import subprocess
import tempfile

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--sdk", type=Path)
    parser.add_argument("--locator", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--fault-point", type=int)
    parser.add_argument("--sample", type=Path)
    parser.add_argument("--sample-sha256")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[5]
    component = repo / "native_component_src/bk7258/ap/h2_pal_core"
    locator = json.loads(args.locator.read_text()) if args.locator else None
    sdk = args.sdk or Path(locator["paths"]["checkout"] if locator else os.environ["BK7258_PATH"])
    expected = (repo / "tools/bazel/native_versions/bk7258_sdk_commit.txt").read_text().strip()
    actual = subprocess.check_output(["git", "-C", str(sdk), "rev-parse", "HEAD"], text=True).strip()
    assert actual == expected, (actual, expected)
    assert not subprocess.check_output(["git", "-C", str(sdk), "status", "--porcelain=v1", "--untracked-files=no"], text=True)
    fdb = sdk / "ap/components/flashdb"
    sources = [fdb / p for p in ("src/fdb.c", "src/fdb_kvdb.c", "src/fdb_utils.c",
        "port/fal/src/fal.c", "port/fal/src/fal_flash.c", "port/fal/src/fal_partition.c")]
    compiler = os.environ.get("CC") or shutil.which("cc")
    assert compiler, "A native C compiler is required"
    if args.work_dir: args.work_dir.mkdir(parents=True,exist_ok=True)
    context = contextlib.nullcontext(str(args.work_dir)) if args.work_dir else tempfile.TemporaryDirectory(prefix="h2-pref-real-nor-")
    with context as tmp:
        binary = Path(tmp) / "pref_nor"
        generator = runpy.run_path(str(component / "src/flashdb/h2_fdb_gc_overlay.py"))
        original = generator["patched_source"](sources[1])
        patched = Path(tmp) / "fdb_kvdb.c"
        patched.write_text(original)
        compile_sources = [patched if p == sources[1] else p for p in sources]
        includes = [component / "tests/pref_nor_sdk", component / "include",
            component / "src/flashdb", component / "src", fdb / "inc", fdb / "port/fal/inc", repo / "libs/pal/include"]
        cmd = [compiler, *(["-fsanitize=address", "-fno-omit-frame-pointer"] if os.environ.get("H2_PREF_NOR_SANITIZE") else []), *(["-DH2_PREF_NOR_DEBUG", "-DFDB_DEBUG_ENABLE"] if os.environ.get("H2_PREF_NOR_DEBUG") else []), "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Wno-unused-parameter",
            *["-I" + str(p) for p in includes], *map(str, compile_sources),
            str(component / "src/h2_bk_platform_pref_flashdb_port.c"),
            str(component / "tests/test_h2_bk_pref_real_nor.c"), "-o", str(binary)]
        subprocess.run(cmd, check=True, timeout=60)
        image = Path(tmp) / "flash.bin"
        for phase in ("seed", "verify", "clear", "clean", "clean"):
            subprocess.run([str(binary), str(image), phase], check=True, timeout=90)
        for phase in ("fault-seed", "fault", "fault-verify", "unknown-tail"):
            subprocess.run([str(binary), str(Path(tmp) / ("fault.bin" if phase != "unknown-tail" else "unknown.bin")), phase], check=True, timeout=90)
        baseline = Path(tmp) / "fault-baseline.bin"
        subprocess.run([str(binary), str(baseline), "fault-seed"], check=True, timeout=90)
        points = [args.fault_point] if args.fault_point else list(range(1, 65)) + [80, 100, 150, 200, 350, 500, 800, 1100]
        reached = 0
        for point in points:
            fault_image = Path(tmp) / "fault-sweep.bin"
            shutil.copyfile(baseline, fault_image)
            for phase in ("fault-nth-" + str(point), "fault-any-verify", "clean"):
                result = subprocess.run([str(binary), str(fault_image), phase], text=True,
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=90)
                if phase.startswith("fault-nth-") and "reached=1" in result.stdout: reached += 1
                if result.returncode:
                    print("Fault point",point,"phase",phase,"\n"+result.stdout,flush=True)
                    result.check_returncode()
        recovery_groups = [("fault-live", "clean"), ("fault-read", "clean"),
            ("fault-erase-1", "fault-any-verify", "clean"),
            ("fault-erase-2", "fault-any-verify", "clean")]
        recovery_groups += [("fault-type-"+str(n),"fault-type-verify","clean") for n in (1,6,12,20,35,64)]
        for phases in recovery_groups:
            fault_image = Path(tmp) / "fault-extra.bin"
            shutil.copyfile(baseline, fault_image)
            for phase in phases:
                result=subprocess.run([str(binary),str(fault_image),phase],text=True,
                    stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=90)
                if result.returncode:
                    print("Recovery group",phases,"phase",phase,"\n"+result.stdout,flush=True);result.check_returncode()
        print("PASS same-process/read recovery, erase-before/after errors, and six type-change fault points")
        assert reached >= min(64,len(points)), reached
        print("PASS",len(points),"injection positions;",reached,"actual NOR IO failures with fresh recovery and cleanup")
        if args.sample:
            sample = args.sample.read_bytes()
            sample_sha = hashlib.sha256(sample).hexdigest()
            if args.sample_sha256: assert sample_sha == args.sample_sha256
            assert len(sample)==0x20000 and sample[0x6000:]==bytes([255])*0x1a000
            # Private disposable copy; never modify or print the original values.
            copied=Path(tmp)/"private-compatible.bin"
            raw=bytearray([255])*0x800000
            raw[0x780000:0x7a0000]=sample
            copied.write_bytes(raw)
            canonical=[]
            for phase in ("compat","compat-verify"):
                export=Path(tmp)/("private-canonical-"+phase+".bin")
                subprocess.run([str(binary),str(copied),phase,str(export)],check=True,timeout=90)
                canonical.append(hashlib.sha256(export.read_bytes()).hexdigest())
            assert canonical[0]==canonical[1]
            print(json.dumps({"private_sample_sha256":sample_sha,"canonical_before_sha256":canonical[0],
                "canonical_after_restart_sha256":canonical[1],"original_sample_modified":False},sort_keys=True))
        print(json.dumps({"sdk_commit":actual,"engine":"actual FlashDB 1.1.2 + FAL with production GC error fix",
            "nor":"4 KiB erase, 1-to-0 writes, fresh processes",
            "sdk_source_sha256":{str(p.relative_to(sdk)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sources}}, sort_keys=True))

if __name__ == "__main__":
    main()
