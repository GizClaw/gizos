"""Compare independent protocol code with the pinned Xtensa SDK under QEMU."""

import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile


def digest(data):
    return hashlib.sha256(data).hexdigest()


def archive_members(data):
    if not data.startswith(b"!<arch>\n"):
        raise ValueError("Expected an ordinary ar archive")
    offset, names = 8, b""
    while offset + 60 <= len(data):
        header = data[offset:offset + 60]
        if header[58:] != b"`\n":
            raise ValueError("Invalid ar member header")
        size = int(header[48:58])
        body = data[offset + 60:offset + 60 + size]
        if len(body) != size:
            raise ValueError("Truncated ar member")
        name = header[:16].decode().strip()
        offset += 60 + size + size % 2
        if name == "//":
            names = body
            continue
        if name in ("/", "/SYM64/") or name.startswith("__.SYMDEF"):
            continue
        if name.startswith("#1/"):
            length = int(name[3:])
            name, body = body[:length].rstrip(b"\0").decode(), body[length:]
        elif name.startswith("/") and name[1:].isdigit():
            start = int(name[1:])
            end = names.find(b"/\n", start)
            if end < 0:
                raise ValueError("Invalid GNU ar name")
            name = names[start:end].decode()
        else:
            name = name.rstrip("/")
        yield name, body


def instruction_hashes(data):
    if data[:6] != b"\x7fELF\x01\x01":
        raise ValueError("Expected little-endian ELF32")
    fields = struct.unpack_from("<16sHHIIIIIHHHHHH", data)
    section_offset, entry_size, count, names_index = (
        fields[6], fields[11], fields[12], fields[13])
    sections = [
        struct.unpack_from("<10I", data, section_offset + index * entry_size)
        for index in range(count)
    ]
    names_section = sections[names_index]
    names = data[names_section[4]:names_section[4] + names_section[5]]
    hashes = {}
    for section in sections:
        name = names[section[0]:].split(b"\0", 1)[0].decode()
        if section[2] & 4 or name.startswith(".literal"):
            hashes[name] = digest(data[section[4]:section[4] + section[5]])
    return hashes


def run(command, directory, label, timeout=60):
    try:
        result = subprocess.run(command, cwd=directory, capture_output=True,
                                timeout=timeout, check=False)
    except subprocess.TimeoutExpired as error:
        (directory / (label + ".stdout")).write_bytes(error.stdout or b"")
        (directory / (label + ".stderr")).write_bytes(error.stderr or b"")
        raise RuntimeError(label + " exceeded its deadline") from error
    (directory / (label + ".stdout")).write_bytes(result.stdout)
    (directory / (label + ".stderr")).write_bytes(result.stderr)
    if result.returncode != 0:
        raise RuntimeError(
            label + " failed:\n" +
            (result.stdout + result.stderr).decode(errors="replace")[-6000:])
    return result


def dependency_root(runfiles, suffix, environment):
    supplied = os.environ.get(environment)
    if supplied:
        root = Path(supplied).resolve()
        if (root / suffix).is_file():
            return root
        raise ValueError(environment + " does not contain " + suffix)
    for candidate in runfiles.iterdir():
        if candidate.is_dir() and (candidate / suffix).is_file():
            return candidate
    raise FileNotFoundError("Missing declared input " + suffix)


def main():
    root = Path(__file__).absolute().parents[4]
    runfiles = Path(os.environ.get("TEST_SRCDIR", root.parent))
    sdk = dependency_root(
        runfiles, "fwk/network/lib/libfwk_network_esp32s3_gcc16.a",
        "ALIPAY_SDK_PATH")
    yyjson = dependency_root(
        runfiles, "src/src/yyjson.c", "ALIPAY_YYJSON_ROOT")
    compiler = os.environ.get("ALIPAY_XTENSA_CC")
    qemu = os.environ.get("ALIPAY_QEMU")
    if not compiler or not qemu:
        raise ValueError("Set ALIPAY_XTENSA_CC and ALIPAY_QEMU for this manual test")
    compiler, qemu = Path(compiler).resolve(), Path(qemu).resolve()
    objcopy = compiler.with_name(compiler.name.removesuffix("gcc") + "objcopy")
    if not all(path.is_file() for path in (compiler, objcopy, qemu)):
        raise FileNotFoundError("Missing caller-supplied Xtensa/QEMU tool")
    directory = Path(os.environ.get(
        "TEST_UNDECLARED_OUTPUTS_DIR", tempfile.mkdtemp(prefix="alipay-differential-")))
    directory.mkdir(parents=True, exist_ok=True)
    manifest = json.loads(
        (root / "third_party/alipay/sdk_manifest.json").read_text())
    selected = [
        "3rdparty/sm3/lib/lib3rdparty_sm3_esp32s3_gcc16.a",
        "3rdparty/cJSON/lib/lib3rdparty_cJSON_esp32s3_gcc16.a",
        "3rdparty/protobuf-c/lib/lib3rdparty_protobuf-c_esp32s3_gcc16.a",
        "3rdparty/mbedtls/lib/lib3rdparty_mbedtls_esp32s3_gcc16.a",
        "fwk/crypt/lib/libfwk_crypt_esp32s3_gcc16.a",
        "fwk/common/lib/libfwk_common_esp32s3_gcc16.a",
        "fwk/network/lib/libfwk_network_esp32s3_gcc16.a",
        "share/common/lib/libshare_common_esp32s3_gcc16.a",
        "app/bind_net/lib/libapp_bind_net_esp32s3_gcc16.a",
        "fwk/secure_mode_ecdh/lib/libfwk_secure_mode_ecdh_esp32s3_gcc16.a",
    ]
    originals = {}
    for name in selected:
        data = (sdk / name).read_bytes()
        if digest(data) != manifest[name]:
            raise ValueError("SDK manifest mismatch: " + name)
        originals[name] = data
    network = "fwk/network/lib/libfwk_network_esp32s3_gcc16.a"
    bindings = "app/bind_net/lib/libapp_bind_net_esp32s3_gcc16.a"
    secure = "fwk/secure_mode_ecdh/lib/libfwk_secure_mode_ecdh_esp32s3_gcc16.a"
    copies = [
        (network, "rpc_client.c.obj", [
            "--globalize-symbol=rpc_init_cfg",
            "--globalize-symbol=rpc_time_to_string"]),
        (network, "alipay_network_util.c.obj", [
            "--redefine-sym=alipay_send_rpc_simple=oracle_original_alipay_send_rpc_simple"]),
        (network, "ant_iot_device_name.c.obj", [
            "--redefine-sym=iot_rpc_send=oracle_send_registration"]),
        (network, "InternalRpcModel.pb3.pb-c.c.obj", []),
        (network, "main_mqtt.c.obj", ["--globalize-symbol=BOX_device_id"]),
        (bindings, "bind_net.c.obj", []),
        (secure, "ecdh_bind.c.obj", [
            "--redefine-sym=IFAA_ecc_generate=oracle_generate",
            "--redefine-sym=IFAA_ecc_set_rng_function=oracle_set_rng"]),
    ]
    objects, instrumentation = [], []
    for archive, member, options in copies:
        members = dict(archive_members(originals[archive]))
        data = members[member]
        original = directory / member
        original.write_bytes(data)
        target = original
        if options:
            target = directory / (member + ".test.o")
            run([str(objcopy), *options, str(original), str(target)],
                directory, member + ".visibility")
            if instruction_hashes(data) != instruction_hashes(target.read_bytes()):
                raise ValueError("Test instrumentation changed instruction bytes")
        instrumentation.append({
            "archive": archive, "member": member, "original_sha256": digest(data),
            "options": options, "instruction_sections_unchanged": True,
        })
        objects.append(str(target))
    sources = [
        "libs/clients/alipay_independent/tests/test_protocol_differential.c",
        "libs/clients/alipay_independent/src/h2_alipay_protocol.c",
        "libs/crypto/sha256/src/h2_sha256.c",
        "libs/crypto/sm3/src/h2_sm3.c",
        "libs/encoding/src/h2_encoding.c",
        "libs/encoding/src/h2_encoding_tables.c",
    ]
    includes = [
        "libs/clients/alipay_independent/include", "libs/crypto/sha256/include",
        "libs/crypto/sm3/include", "libs/encoding/include", "pal/include",
    ]
    elf = directory / "protocol-differential.elf"
    command = [
        str(compiler), "-std=c11", "-Os", "-Wall", "-Wextra", "-Werror",
        "-mlongcalls", "-ffunction-sections", "-fdata-sections",
        "--specs=sim.elf.specs", "-DYYJSON_DISABLE_FAST_FP_CONV=1",
        *["-I" + str(root / name) for name in includes],
        "-I" + str(yyjson / "src/src"),
        *[str(root / name) for name in sources],
        str(yyjson / "src/src/yyjson.c"), *objects,
        "-Wl,--start-group",
        *[str(sdk / name) for name in selected if name not in (bindings, secure)],
        "-Wl,--end-group", "-Wl,--gc-sections", "-o", str(elf),
    ]
    run(command, directory, "build", timeout=90)
    result = run([
        str(qemu), "-M", "esp32s3", "-nographic", "-monitor", "none",
        "-nic", "none", "-semihosting", "-kernel", str(elf),
    ], directory, "qemu", timeout=45)
    summary = json.loads(result.stdout.decode().strip().splitlines()[-1])
    if summary["failed"] != 0 or summary["original_binary_comparisons"] < 2295:
        raise ValueError("Incomplete original-SDK differential run")
    for name, original in originals.items():
        if digest((sdk / name).read_bytes()) != digest(original):
            raise ValueError("Original SDK archive changed: " + name)
    summary.update({
        "network": "disabled",
        "inputs": "synthetic identities, tokens and controlled public points",
        "original_archives_sha256": {
            name: digest(data) for name, data in originals.items()},
        "test_copy_instrumentation": instrumentation,
        "elf_sha256": digest(elf.read_bytes()),
        "source_sha256": {
            name: digest((root / name).read_bytes()) for name in sources},
    })
    (directory / "differential-result.json").write_text(
        json.dumps(summary, indent=2) + "\n")
    print(json.dumps({name: summary[name] for name in
                      ("original_binary_comparisons", "failed", "network")}))


if __name__ == "__main__":
    main()
