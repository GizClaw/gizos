"""Run selected SDK Flash locks with deterministic AP/APX/CP contention."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def function(source, name):
    match = re.search(r"^static void " + name + r"\(void\)\s*\{", source, re.M)
    assert match, name
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


def reversed_lock_order(source):
    """Reintroduce the old cycle without depending on shallow Git history."""
    fixed_entry = """\tuint32_t flags = rtos_disable_int();
\tuint32_t cpu_id = bk_flash_shared_get_cpu_id();

\tif (bk_flash_shared_acquire(cpu_id) != BK_OK) {
\t\trtos_enable_int(flags);
\t\tBK_ASSERT(0);
\t}
#ifdef CONFIG_FREERTOS_SMP
\tspin_lock(&s_flash_aspl_spin_lock);
#endif

\treturn flags;"""
    old_entry = """\tuint32_t flags = rtos_disable_int();
#ifdef CONFIG_FREERTOS_SMP
\tspin_lock(&s_flash_aspl_spin_lock);
#endif
\tuint32_t cpu_id = bk_flash_shared_get_cpu_id();

\tif (bk_flash_shared_acquire(cpu_id) != BK_OK) {
#ifdef CONFIG_FREERTOS_SMP
\t\tspin_unlock(&s_flash_aspl_spin_lock);
#endif
\t\trtos_enable_int(flags);
\t\tBK_ASSERT(0);
\t}

\treturn flags;"""
    fixed_exit = """#ifdef CONFIG_FREERTOS_SMP
\tspin_unlock(&s_flash_aspl_spin_lock);
#endif
\tbk_flash_shared_release(cpu_id);
\trtos_enable_int(flags);"""
    old_exit = """\tbk_flash_shared_release(cpu_id);
#ifdef CONFIG_FREERTOS_SMP
\tspin_unlock(&s_flash_aspl_spin_lock);
#endif
\trtos_enable_int(flags);"""
    assert source.count(fixed_entry) == 1, "Revalidate SDK Flash critical entry"
    assert source.count(fixed_exit) == 1, "Revalidate SDK Flash critical exit"
    return source.replace(fixed_entry, old_entry).replace(fixed_exit, old_exit)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--sdk", type=Path)
    parser.add_argument("--locator", type=Path)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[5]
    component = repo / "native_component_src/bk7258/ap/h2_pal_core"
    locator = json.loads(args.locator.read_text()) if args.locator else None
    sdk = args.sdk or Path(locator["paths"]["checkout"] if locator else os.environ["BK7258_PATH"])
    expected = (repo / "tools/bazel/native_versions/bk7258_sdk_commit.txt").read_text().strip()
    actual = subprocess.check_output(["git", "-C", str(sdk), "rev-parse", "HEAD"], text=True).strip()
    assert actual == expected, (actual, expected)
    assert not subprocess.check_output(
        ["git", "-C", str(sdk), "status", "--porcelain", "--untracked-files=no"], text=True)
    shared_path = sdk / "ap/middleware/driver/flash/flash_shared_lock.c"
    before = hashlib.sha256(shared_path.read_bytes()).hexdigest()
    fixed_source = shared_path.read_text()
    driver = (shared_path.parent / "flash_driver.c").read_text()
    driver_lock = function(driver, "flash_lock") + "\n" + function(driver, "flash_unlock")
    probe = (component / "tests/flash_lock_order_probe.c").read_text()
    cc = shutil.which("cc")
    assert cc
    with tempfile.TemporaryDirectory(prefix="h2-flash-lock-order-") as temporary:
        work = Path(temporary)
        for name, source in [("negative", reversed_lock_order(fixed_source)),
                             ("sdk", fixed_source)]:
            shared = re.sub(r"^#include[^\n]*\n", "", source, flags=re.M)
            shared = shared.replace('__asm volatile("dmb sy" ::: "memory");',
                                    "atomic_thread_fence(memory_order_seq_cst);")
            # MMIO fields have independent core writers. Use atomic host
            # storage for the hardware's ordered volatile word accesses.
            shared = shared.replace("volatile uint32_t", "_Atomic uint32_t")
            unit, binary = work / (name + ".c"), work / name
            unit.write_text(probe.replace("/* SDK_SHARED_LOCK */", shared)
                            .replace("/* SDK_DRIVER_LOCK */", driver_lock))
            command = [cc, "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                       "-pthread", str(unit), "-o", str(binary)]
            if args.sanitize and name == "sdk":
                command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            subprocess.run(command, check=True, timeout=60)
            subprocess.run([str(binary)], check=True, timeout=10)
            run = subprocess.run([str(binary), "overlap"], capture_output=True, text=True, timeout=10)
            if name == "negative":
                assert run.returncode == 86 and "LOCK_ORDER_CYCLE" in run.stderr, run
            else:
                assert run.returncode == 0, run.stdout + run.stderr
                print(run.stdout, end="")
    assert hashlib.sha256(shared_path.read_bytes()).hexdigest() == before
    print(json.dumps({"result": "PASS", "sdk_head": actual,
                      "reversed_order_negative_control": "lock cycle",
                      "sdk": "all workers joined and locks released",
                      "sanitize": args.sanitize}, sort_keys=True))


if __name__ == "__main__":
    main()
