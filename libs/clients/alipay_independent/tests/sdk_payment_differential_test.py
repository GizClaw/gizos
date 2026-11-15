"""Compare synthetic payer-code vectors and query cases with original Xtensa code."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

def execute(args,directory,name,env=None):
    result=subprocess.run(args,cwd=directory,env=env,capture_output=True,timeout=120)
    (directory/(name+".stdout")).write_bytes(result.stdout)
    (directory/(name+".stderr")).write_bytes(result.stderr)
    if result.returncode:
        raise RuntimeError(name+" failed:\n"+(result.stdout+result.stderr).decode(errors="replace")[-6500:])
    return result

def main():
    root=Path(__file__).absolute().parents[4]
    sdk=Path(os.environ["ALIPAY_SDK_PATH"]).resolve()
    compiler=Path(os.environ["ALIPAY_XTENSA_CC"]).resolve()
    qemu=Path(os.environ["ALIPAY_QEMU"]).resolve()
    directory=Path(os.environ.get("TEST_UNDECLARED_OUTPUTS_DIR",
                                 tempfile.mkdtemp(prefix="alipay-payment-diff-")))
    directory.mkdir(parents=True,exist_ok=True)
    manifest=json.loads((root/"third_party/alipay/sdk_manifest.json").read_text())
    selected=[
        "app/pay_code/lib/libapp_pay_code_esp32s3_gcc16.a",
        "app/pay_result/lib/libapp_pay_result_esp32s3_gcc16.a",
        "fwk/secure_mode_ecdh/lib/libfwk_secure_mode_ecdh_esp32s3_gcc16.a",
        "fwk/crypt/lib/libfwk_crypt_esp32s3_gcc16.a",
        "3rdparty/mbedtls/lib/lib3rdparty_mbedtls_esp32s3_gcc16.a",
        "3rdparty/cJSON/lib/lib3rdparty_cJSON_esp32s3_gcc16.a",
    ]
    originals={}
    for name in selected:
        data=(sdk/name).read_bytes()
        digest=hashlib.sha256(data).hexdigest()
        assert digest==manifest[name],name
        originals[name]=digest
    version=execute([str(compiler),"-dumpversion"],directory,"compiler").stdout.decode().strip()
    target_lib=compiler.parent.parent/"xtensa-esp-elf/lib/esp32s3"
    gcc_lib=compiler.parent.parent/"lib/gcc/xtensa-esp-elf"/version/"esp32s3"
    includes=[
        root/"libs/clients/alipay_independent/include",
        root/"libs/clients/alipay_independent/src",
        root/"pal/include",
        sdk/"share/common/inc",sdk/"app/pay_code/inc",sdk/"app/pay_result/inc",
        sdk/"hal/device_info/inc",
    ]
    sources=[
        root/"libs/clients/alipay_independent/tests/test_payment_differential.c",
        root/"libs/clients/alipay_independent/src/h2_alipay_paycode.c",
    ]
    elf=directory/"payment-differential.elf"
    command=[
        str(compiler),"-std=c11","-Os","-Wall","-Wextra","-Werror",
        "-mdynconfig="+str(compiler.parent.parent/"lib/xtensa_esp32s3.so"),
        "-B"+str(target_lib)+"/","-B"+str(gcc_lib)+"/",
        "-mlongcalls","-ffunction-sections","-fdata-sections","--specs=sim.elf.specs",
        *["-I"+str(p) for p in includes],*[str(p) for p in sources],
        "-Wl,--start-group",*[str(sdk/n) for n in selected],
        "-Wl,--end-group","-Wl,--gc-sections","-o",str(elf),
    ]
    execute(command,directory,"build")
    env=dict(os.environ)
    if env.get("ALIPAY_QEMU_LIBDIR"):
        env["DYLD_LIBRARY_PATH"]=env["ALIPAY_QEMU_LIBDIR"]
    result=execute([str(qemu),"-M","esp32s3","-nographic","-monitor","none",
                    "-nic","none","-semihosting","-kernel",str(elf)],
                   directory,"qemu",env)
    summary=json.loads(result.stdout.decode().strip().splitlines()[-1])
    assert summary["failed"]==0 and summary["original_binary_comparisons"]>=1682
    assert summary["query_comparisons"]>=9
    for name,digest in originals.items():
        assert hashlib.sha256((sdk/name).read_bytes()).hexdigest()==digest
    summary.update({
        "network":"disabled",
        "materials":"synthetic keys/seeds/identities; no device material",
        "original_archives_sha256":originals,
        "original_executable_sections_modified":False,
        "fixture_boundaries":[
            "PSA read, current time, MAC, bound state, last displayed code",
            "RPC transport and remote-unbind callback",
            "CBC/HMAC primitives for candidate use original crypto objects",
            "pure ECDH get-paycode dispatch forwards to original get_paycode",
        ],
        "elf_sha256":hashlib.sha256(elf.read_bytes()).hexdigest(),
        "source_sha256":{str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest()
                         for p in sources},
    })
    (directory/"differential-result.json").write_text(json.dumps(summary,indent=2)+"\n")
    print(json.dumps({k:summary[k] for k in
        ("original_binary_comparisons","query_comparisons","failed","network")}))

if __name__=="__main__":main()
