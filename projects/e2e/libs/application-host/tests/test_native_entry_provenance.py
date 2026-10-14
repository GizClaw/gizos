import copy
import hashlib
import json
from pathlib import Path
import unittest
from unittest.mock import patch
import native_entry_provenance as provenance

class NativeEntryProvenance(unittest.TestCase):
    def setUp(self):
        self.audit = json.loads(provenance.RECORD.read_text())
        self.path = "projects/e2e/targets/h2loader_tar_zlib/pal-audio/amoled/main/main.c"
        self.expected = self.audit["source_changes"][self.path]["previous_sha256"]
        self.current = Path(self.path).read_text()

    def verify(self, audit=None):
        with patch.object(provenance.json.JSONDecoder, "decode", return_value=audit or self.audit):
            provenance.qualified_source(self.path, self.expected)

    def test_old_device_receipt_and_new_assembly_are_separate(self):
        self.verify()
        self.assertEqual(self.audit["current_native_qualification"], "NOT_RUN")
        self.assertFalse(self.audit["new_physical_run_claimed"])

    def test_claimed_device_run_or_unknown_scope_is_rejected(self):
        for field, value in [("new_physical_run_claimed", True), ("current_native_qualification", "PASS")]:
            audit=copy.deepcopy(self.audit);audit[field]=value
            with self.assertRaises(AssertionError):self.verify(audit)
        audit=copy.deepcopy(self.audit)
        audit["source_changes"]["projects/e2e/apps/pal-audio/app/src/h2_pal_audio_e2e.c"]={}
        with self.assertRaises(AssertionError):self.verify(audit)

    def test_changed_prior_or_current_hash_is_rejected(self):
        for key in ["previous_sha256", "current_sha256", "protected_suite_sha256"]:
            audit=copy.deepcopy(self.audit);audit["source_changes"][self.path][key]="0"*64
            with self.assertRaises(AssertionError):self.verify(audit)

    def test_case_budget_change_cannot_be_labeled_an_entry_refactor(self):
        changed=self.current.replace(".stability_ms = 30000u", ".stability_ms = 1u")
        self.assertNotEqual(changed, self.current)
        self.assertNotEqual(provenance.suite_source(changed), provenance.suite_source(self.current))
        audit=copy.deepcopy(self.audit)
        audit["source_changes"][self.path]["current_sha256"]=hashlib.sha256(changed.encode()).hexdigest()
        read_bytes=Path.read_bytes;read_text=Path.read_text
        def content_bytes(path, *args, **kwargs):
            return changed.encode() if str(path)==self.path else read_bytes(path, *args, **kwargs)
        def content_text(path, *args, **kwargs):
            return changed if str(path)==self.path else read_text(path, *args, **kwargs)
        with patch.object(Path,"read_bytes",content_bytes), patch.object(Path,"read_text",content_text):
            with self.assertRaises(AssertionError):self.verify(audit)

if __name__=="__main__":unittest.main()
