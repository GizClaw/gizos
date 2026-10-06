"""Apply reviewed error/recovery fixes to the pinned SDK without editing it."""
import argparse
import hashlib
from pathlib import Path

SOURCE_SHA256 = "d418968a491c7a6f92ab007996d2da90fe17436e00a91c8f696bdada62ad40f3"

def patched_source(source: Path) -> str:
    raw = source.read_bytes()
    if hashlib.sha256(raw).hexdigest() != SOURCE_SHA256:
        raise ValueError("FlashDB overlay requires the audited BK SDK source")
    text = raw.decode("utf-8")
    folder = Path(__file__).resolve().parent
    for name, start, end in (
        ("move_kv", "\nstatic fdb_err_t move_kv(", "\nstatic uint32_t new_kv("),
        ("do_gc", "\nstatic bool do_gc(", "\n/*\n * The GC will be triggered"),
    ):
        begin, finish = text.index(start), text.index(end)
        replacement = (folder / f"h2_fdb_{name}.inc").read_text()
        text = text[:begin] + "\n" + replacement + text[finish:]
    replacements = (
        # Keep del_kv's local object alive until its escaped pointer is done.
        ("    uint32_t dirty_status_addr;\n", "    uint32_t dirty_status_addr;\n    struct fdb_kv kv;\n"),
        ("        struct fdb_kv kv;\n        /* find KV */", "        /* find KV */"),
        # Invalid PRE_WRITE must not stop recovery of later PRE_DELETE records.
        ("        return true;\n    } else if (kv->crc_is_ok && kv->status == FDB_KV_WRITE)",
         "        return false;\n    } else if (kv->crc_is_ok && kv->status == FDB_KV_WRITE)"),
        # During recovery a complete relocation can consume the GC reserve.
        ("empty_sector > FDB_GC_EMPTY_SEC_THRESHOLD || db->gc_request)",
         "empty_sector > FDB_GC_EMPTY_SEC_THRESHOLD || db->gc_request || db->in_recovery_check)"),
        # Same meaning on ARM32; use the field's width on the LP64 host as well.
        ("kv->len == ~0UL", "kv->len == UINT32_MAX"),
    )
    for old, new in replacements:
        if text.count(old) != 1:
            raise ValueError("FlashDB overlay anchor changed")
        text = text.replace(old, new)
    return text

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.write_text(patched_source(args.input))

if __name__ == "__main__":
    main()
