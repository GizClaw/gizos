# BK MQTT qualification import

`import_qualification.py` is a read-only evidence consumer. It never opens UART, starts or stops a fixture, installs firmware or changes captures. Run it only after the directed owner reports the new run qualified. It requires actual `collector-receipt.json` with exact `qualified=true`, integer `strict_verifier_exit=0`, unchanged log hashes, clean source boundary, successful build receipt, matching immutable package/binding and the expected version/source. It reruns the exact recorded verifier in a temporary directory and requires identical qualification output. It separately validates the first complete READY for each boot, the strengthened retained received=2/publish=2/disconnect=1 witness and the preserved original 32-byte coredump SHA256.

```sh
python3 projects/e2e/targets/h2loader_tar_zlib/pal-mqtt/bk7258_v3_202405/tools/import_qualification.py \
  --evidence-directory /absolute/private/mqtt-bk-r11 \
  --output-directory projects/e2e/targets/h2loader_tar_zlib/pal-mqtt/bk7258_v3_202405/evidence/runs/7ff2a427-r11 \
  --verifier-repo /absolute/path/to/the/recorded/host/checkout \
  --expected-source 7ff2a42794bf8509416e8de62581fd717712178d \
  --expected-version mqtt-bk-paced-r11 \
  --expected-uid c8478ca2a87c \
  --expected-coredump-sha256 <actual-original-32-byte-sha256>
```

The destination must not exist; previous R9/R10 failures cannot be overwritten. Only normative JSON, protocol witnesses, receipts and hashes are copied. Raw native diagnostics, storage data and coredump bytes remain private. Imported qualification explicitly records `restored=false`; the directed owner must separately supply actual final restore/status/dump evidence before changing that field. The artifact source remains the actual packaged source, independent of this host-only utility's later commit.

The format/admission unit tests are not E2E results:

```sh
python3 -m unittest discover \
  -s projects/e2e/targets/h2loader_tar_zlib/pal-mqtt/bk7258_v3_202405/tools \
  -p test_import_qualification.py -v
```

When a new capture reuses an immutable device package, pass `--artifact-build-receipt /absolute/path/to/the/original/build-receipt.json`. Its successful exit, original source and actual version are still verified and the original receipt is copied unchanged. This does not assert a new device build. The capture, new host build/source/binary, exact verifier snapshot/hash and subsequent restoration remain separate evidence identities.
