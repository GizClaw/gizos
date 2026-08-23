#!/usr/bin/env python3

import binascii
import pathlib
import struct
import subprocess
import sys
import tempfile


def crc_line(block: bytes) -> bytes:
    crc = 0xFFFF
    for byte in block:
        for bit_index in range(7, -1, -1):
            data_bit = (byte >> bit_index) & 1
            top_bit = (crc >> 15) & 1
            crc = ((crc << 1) & 0xFFFF) | data_bit
            crc ^= (data_bit << 15) | (data_bit << 2)
            crc ^= (top_bit << 15) | (top_bit << 2) | top_bit
    return block + crc.to_bytes(2, "big")


def image_crc(data: bytes) -> int:
    # binascii uses the complementary initial/final convention. Complementing
    # both sides produces the BK3633 CRC32 (initial 0xffffffff, no final xor).
    return binascii.crc32(data, 0) ^ 0xFFFFFFFF


def header(payload: bytes, version: int, rom_version: int, uid: int) -> bytes:
    package_size = 16 + len(payload)
    return struct.pack(
        "<IHHIBBH",
        image_crc(payload),
        version,
        package_size // 4,
        uid,
        0xFF,
        0xFF,
        rom_version,
    )


def expected_image(
    boot: bytes,
    stack: bytes,
    app: bytes,
    stack_address: int,
    app_address: int,
    version: int,
    rom_version: int,
) -> bytes:
    logical = bytearray(b"\xff" * (app_address + len(app)))
    logical[: len(boot)] = boot
    logical[stack_address : stack_address + len(stack)] = stack
    logical[app_address:] = app
    logical.extend(b"\xff" * (-len(logical) % 32))
    image = bytearray(
        b"".join(crc_line(logical[offset : offset + 32]) for offset in range(0, len(logical), 32))
    )
    image[0x130:0x132] = b"\x00\x00"
    stack_header_offset = stack_address * 34 // 32 - 16
    app_header_offset = app_address * 34 // 32 - 16
    image.extend(b"\xff" * (256 - ((len(image) - stack_header_offset) & 0xFF)))
    image[app_header_offset : app_header_offset + 16] = header(
        image[app_header_offset + 16 :], version, rom_version, 0x42424242
    )
    image[stack_header_offset : stack_header_offset + 16] = header(
        image[stack_header_offset + 16 :], version, rom_version, 0x53535353
    )
    return bytes(image)


def run_converter(
    executable: pathlib.Path,
    directory: pathlib.Path,
    stack_address: int,
    app_address: int,
    version: int,
    rom_version: int,
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        converter_args(
            executable,
            directory,
            stack_address,
            app_address,
            version,
            rom_version,
        ),
        check=True,
        text=True,
        capture_output=True,
    )


def converter_args(
    executable: pathlib.Path,
    directory: pathlib.Path,
    stack_address: int,
    app_address: int,
    version: int,
    rom_version: int,
) -> list[str]:
    return [
        str(executable),
        "-oad",
        str(directory / "boot.bin"),
        str(directory / "stack.bin"),
        str(directory / "app.bin"),
        "-m",
        hex(stack_address),
        "-l",
        hex(app_address),
        "-v",
        hex(version),
        "-rom_v",
        hex(rom_version),
        "-e",
        "00000000",
        "00000000",
        "00000000",
        "00000000",
    ]


def expect_failure(
    arguments: list[str], directory: pathlib.Path, expected_error: str
) -> None:
    result = subprocess.run(
        arguments,
        check=False,
        text=True,
        capture_output=True,
    )
    assert result.returncode != 0
    assert expected_error in result.stderr
    for name in ("app_merge_crc.bin", "app_oad.bin", "app_stack_oad.bin"):
        assert not (directory / name).exists()


def main() -> None:
    executable = pathlib.Path(sys.argv[1]).resolve()
    stack_address = 0x400
    app_address = 0x900
    version = 0x2A
    rom_version = 0x18
    boot = bytes((index * 17 + 3) & 0xFF for index in range(389))
    stack = bytes((index * 29 + 11) & 0xFF for index in range(733))
    app = bytes((index * 43 + 19) & 0xFF for index in range(1061))
    expected = expected_image(
        boot, stack, app, stack_address, app_address, version, rom_version
    )

    with tempfile.TemporaryDirectory() as directory_name:
        directory = pathlib.Path(directory_name)
        for name, data in (("boot.bin", boot), ("stack.bin", stack), ("app.bin", app)):
            (directory / name).write_bytes(data)
        run_converter(
            executable, directory, stack_address, app_address, version, rom_version
        )
        actual = (directory / "app_merge_crc.bin").read_bytes()
        assert actual == expected
        stack_header_offset = stack_address * 34 // 32 - 16
        app_header_offset = app_address * 34 // 32 - 16
        assert (directory / "app_oad.bin").read_bytes() == expected[app_header_offset:]
        assert (directory / "app_stack_oad.bin").read_bytes() == expected[stack_header_offset:]

    with tempfile.TemporaryDirectory() as directory_name:
        directory = pathlib.Path(directory_name)
        large_app = bytes((index * 7 + 5) & 0xFF for index in range(80_000))
        for name, data in (
            ("boot.bin", boot),
            ("stack.bin", stack),
            ("app.bin", large_app),
        ):
            (directory / name).write_bytes(data)
        (directory / "app_stack_oad.bin").write_bytes(b"stale")
        result = run_converter(
            executable, directory, 0x1F00, 0x2E200, version, rom_version
        )
        image = (directory / "app_merge_crc.bin").read_bytes()
        stack_header_offset = 0x1F00 * 34 // 32 - 16
        app_header_offset = 0x2E200 * 34 // 32 - 16
        assert image[stack_header_offset : stack_header_offset + 16] == b"\xff" * 16
        assert (directory / "app_oad.bin").read_bytes() == image[app_header_offset:]
        assert not (directory / "app_stack_oad.bin").exists()
        assert "16-bit length limit" in result.stdout

    with tempfile.TemporaryDirectory() as directory_name:
        directory = pathlib.Path(directory_name)
        for name, data in (("boot.bin", boot), ("stack.bin", stack), ("app.bin", app)):
            (directory / name).write_bytes(data)

        arguments = converter_args(
            executable, directory, stack_address, app_address, version, rom_version
        )
        expect_failure(arguments + ["--unknown"], directory, "unknown or incomplete option")

        nonzero_key_arguments = arguments.copy()
        nonzero_key_arguments[-1] = "00000001"
        expect_failure(
            nonzero_key_arguments,
            directory,
            "non-zero encryption keys are not supported",
        )

        expect_failure(
            converter_args(
                executable,
                directory,
                stack_address + 1,
                app_address,
                version,
                rom_version,
            ),
            directory,
            "stack and app addresses must be 32-byte aligned",
        )

        expect_failure(
            converter_args(
                executable,
                directory,
                stack_address,
                0x600,
                version,
                rom_version,
            ),
            directory,
            "input images overlap or are too large",
        )

    print("portable BinConvert tests passed")


if __name__ == "__main__":
    main()
