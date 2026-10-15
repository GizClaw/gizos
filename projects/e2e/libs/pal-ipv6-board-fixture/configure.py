"""Mint one temporary two-board session; no host network service is started."""
import argparse
import json
import os
from pathlib import Path
import sys
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "pal-net-tls-fixture"))
from fixture import mint


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    root = Path(args.output).resolve()
    root.mkdir(parents=True, exist_ok=False)
    os.chmod(root, 0o700)
    ca, wrong, leaf, expired, key = mint(root)
    session = uuid.uuid4().hex
    host = "fd53:697a:6f73:626::1"
    public = dict(H2_PAL_IPV6_HOST=host, H2_PAL_IPV6_SESSION=session,
        H2_PAL_IPV6_CA_HEX=ca.read_bytes().hex(),
        H2_PAL_IPV6_WRONG_CA_HEX=wrong.read_bytes().hex(),
        H2_PAL_IPV6_PORT=19006, H2_PAL_IPV6_MQTT_PORT=18883,
        H2_PAL_IPV6_DNS_PORT=15353, H2_PAL_IPV6_DNS_HOST="localhost",
        H2_PAL_IPV6_DNS_IPV6="::1", H2_PAL_IPV6_HTTP_URL=f"http://[{host}]:18080/{session}",
        H2_PAL_IPV6_FALLBACK_URL="", H2_PAL_IPV6_OFFER_URL=f"http://[{host}]:18080/offer",
        H2_PAL_IPV6_STUN_URL=f"stun:[{host}]:13478", H2_PAL_IPV6_EPOCH_MS=int(time.time() * 1000),
        H2_PAL_IPV6_BOARD_FIXTURE=1)
    private = dict(public, H2_PAL_IPV6_LEAF_HEX=leaf.read_bytes().hex(),
        H2_PAL_IPV6_EXPIRED_HEX=expired.read_bytes().hex(), H2_PAL_IPV6_KEY_HEX=key.read_bytes().hex())
    for filename, values in (("fixture.bazelrc", public), ("server.bazelrc", private)):
        path = root / filename
        path.write_text("".join(f"build --define={name}={value}\n" for name, value in values.items()))
        os.chmod(path, 0o600)
    (root / "config.json").write_text(json.dumps(public, indent=2) + "\n")
    for path in root.iterdir():
        if path.is_file():
            os.chmod(path, 0o600)
    print(json.dumps(dict(session=session, host=host, public_config=str(root / "fixture.bazelrc"),
                         server_config=str(root / "server.bazelrc"))))


if __name__ == "__main__":
    main()
