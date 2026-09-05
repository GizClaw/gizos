from __future__ import annotations

from pathlib import Path
import stat
import subprocess
import tempfile
import unittest


SCRIPT = Path(__file__).parents[1] / "jieli" / "local_post_wl82.sh"


def write_executable(path: Path, body: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("#!/bin/bash\nset -eu\n" + body, encoding="utf-8")
    path.chmod(path.stat().st_mode | stat.S_IXUSR)


class JieliWl82PostBuildTest(unittest.TestCase):
    def run_postbuild(
        self, double_bank: bool, omit_factory: bool = False
    ) -> tuple[Path, list[list[str]], list[str]]:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        sdk = root / "sdk"
        tools = sdk / "cpu" / "wl82" / "tools"
        toolchain_bin = root / "toolchain" / "pi32v2" / "bin"
        postbuild = root / "postbuild"
        output = root / "output"
        tools.mkdir(parents=True)
        for name in ("jl_isd.bin", "jl_isd.fw", "jl_isd.ufw", "jl_isd_extend.bin",
                     "db_update_files_data.bin", "db_update_files_data.managed.bin"):
            (tools / name).write_bytes(b"stale SDK output")

        for name in ("sdk.elf", "uboot.boot", "cfg_tool.bin", "script.ver", "ota.bin"):
            (tools / name).write_bytes(name.encode())
        (tools / "cfg").mkdir()
        (tools / "isd_config.ini").write_text(
            ("BR22_TWS_DB=YES;\nDB_UPDATE_DATA=YES;\n" if double_bank else "")
            + "SPI=2_3_0_0;\n",
            encoding="utf-8",
        )

        write_executable(
            toolchain_bin / "objcopy",
            'for last; do :; done\nprintf "%s" "$last" > "$last"\n',
        )
        write_executable(toolchain_bin / "objdump", 'printf "sections\\n"\n')
        write_executable(toolchain_bin / "objsizedump", 'printf "symbols\\n"\n')
        write_executable(
            postbuild / "isd_download",
            f'printf "%s\\n" "$*" >> "{root / "isd.log"}"\n'
            'for old in jl_isd.bin jl_isd.fw jl_isd.ufw jl_isd_extend.bin '
            'db_update_files_data.bin db_update_files_data.managed.bin; do '
            'test ! -e "$old"; done\n'
            'printf "flash" > jl_isd.bin\n'
            'printf "firmware" > jl_isd.fw\n'
            + ('' if omit_factory else 'printf "full factory" > jl_isd_extend.bin\n')
            +
            'case " $* " in *" -update_files normal "*) '
            'printf "double-bank" > db_update_files_data.bin;; esac\n',
        )
        write_executable(
            postbuild / "fw_add",
            f'printf "%s\\n" "$*" >> "{root / "fw_add.log"}"\n',
        )
        write_executable(
            postbuild / "ufw_maker",
            'input=$2\nprintf "ufw" > "${input%.fw}.ufw"\n',
        )

        completed = subprocess.run(
            [
                str(SCRIPT),
                str(sdk),
                str(root / "toolchain"),
                str(postbuild),
                str(output),
            ],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        if omit_factory:
            self.assertNotEqual(completed.returncode, 0)
            self.assertFalse((output / "jl_isd.bin").exists())
            return output, [], []
        self.assertEqual(
            completed.returncode,
            0,
            f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}",
        )
        isd_invocations = [
            line.split()
            for line in (root / "isd.log").read_text(encoding="utf-8").splitlines()
        ]
        fw_add_args = (
            (root / "fw_add.log").read_text(encoding="utf-8").splitlines()
            if (root / "fw_add.log").exists()
            else []
        )
        return output, isd_invocations, fw_add_args

    def test_double_bank_publishes_managed_update_payload(self) -> None:
        output, isd_invocations, fw_add_args = self.run_postbuild(double_bank=True)
        self.assertEqual(len(isd_invocations), 1)
        self.assertIn("-update_files", isd_invocations[0])
        self.assertIn("normal", isd_invocations[0])
        self.assertIn("-extend-bin", isd_invocations[0])
        self.assertEqual((output / "jl_isd.bin").read_bytes(), b"full factory")
        self.assertEqual((output / "update.ufw").read_bytes(), b"double-bank")
        self.assertFalse(any("ota.bin" in invocation for invocation in fw_add_args))
        self.assertTrue(any("script.ver" in invocation for invocation in fw_add_args))

    def test_single_bank_keeps_legacy_ufw_payload(self) -> None:
        output, isd_invocations, fw_add_args = self.run_postbuild(double_bank=False)
        self.assertEqual(len(isd_invocations), 1)
        self.assertNotIn("-update_files", isd_invocations[0])
        self.assertEqual((output / "update.ufw").read_bytes(), b"ufw")
        self.assertTrue(any("ota.bin" in invocation for invocation in fw_add_args))
        self.assertTrue(any("script.ver" in invocation for invocation in fw_add_args))

    def test_stale_factory_image_cannot_mask_missing_output(self) -> None:
        self.run_postbuild(double_bank=True, omit_factory=True)


if __name__ == "__main__":
    unittest.main()
