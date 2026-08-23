import copy
import importlib.util
import pathlib
import tempfile
import unittest


MODULE_PATH = pathlib.Path(__file__).with_name("generate_layout.py")
SPEC = importlib.util.spec_from_file_location("generate_layout", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
layout_module = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(layout_module)


class LayoutValidationTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name)
        (self.root / "data").mkdir()
        self.valid = {
            "display": {"title": "Desktop", "width": 480, "height": 320},
            "filesystem": {
                "mounts": [{"source": "data", "target": "/data"}],
            },
            "peripherals": [
                {"periph_id": 1, "kind": {"button": {"key": "space"}}},
                {
                    "periph_id": 2,
                    "kind": {"nfc_reader": {"simulate": True, "key": "n"}},
                },
            ],
        }

    def validate(self, value):
        return layout_module.validate_layout(
            copy.deepcopy(value), "example/app", self.root)

    def assert_invalid(self, mutate, field):
        value = copy.deepcopy(self.valid)
        mutate(value)
        with self.assertRaisesRegex(layout_module.LayoutError, field):
            self.validate(value)

    def test_valid_layout_defaults_and_header_are_deterministic(self):
        first = self.validate(self.valid)
        second = self.validate(self.valid)
        self.assertEqual(first, second)
        self.assertEqual(
            layout_module.emit_header(first, "example/app"),
            layout_module.emit_header(second, "example/app"),
        )
        self.assertFalse(first["simulation"]["wifi_sta"]["connected"])

    def test_required_and_unknown_fields_fail_closed(self):
        self.assert_invalid(lambda value: value.pop("display"), "display")
        self.assert_invalid(
            lambda value: value["display"].update({"extra": True}), "extra")
        self.assert_invalid(
            lambda value: value["peripherals"][0]["kind"].update(
                {"imu": {"simulate": True}}),
            "exactly one",
        )

    def test_utf8_nul_numeric_and_capacity_boundaries(self):
        self.assert_invalid(
            lambda value: value["display"].update({"title": "bad\0title"}),
            "embedded NUL",
        )
        self.assert_invalid(
            lambda value: value["display"].update({"width": 0}), "width")
        self.assert_invalid(
            lambda value: value["display"].update({"title": "\ud800"}),
            "valid UTF-8",
        )
        self.assert_invalid(
            lambda value: value["peripherals"].append({
                "periph_id": 3,
                "kind": {"battery": {
                    "voltage_mv": 3800,
                    "percent_x100": 10001,
                    "state": "charging",
                }},
            }),
            "percent_x100",
        )
        self.assert_invalid(
            lambda value: value.update({
                "simulation": {
                    "wifi_sta": {
                        "scan_results": [
                            {
                                "ssid": f"ap-{index}",
                                "security": "open",
                            }
                            for index in range(17)
                        ],
                    },
                },
            }),
            "more than 16",
        )

    def test_simulation_constraints(self):
        self.assert_invalid(
            lambda value: value.update({
                "simulation": {
                    "wifi_sta": {"connected": True, "ssid": ""},
                },
            }),
            "requires an SSID",
        )
        self.assert_invalid(
            lambda value: value.update({
                "simulation": {
                    "modem": {
                        "available": False,
                        "mobile_data_enabled": True,
                    },
                },
            }),
            "requires an available modem",
        )
        value = copy.deepcopy(self.valid)
        value["gizclaw"] = {"endpoint": "ap.dev.gizclaw.com:9821"}
        self.assertEqual(
            value["gizclaw"],
            self.validate(value)["gizclaw"],
        )
        self.assert_invalid(
            lambda layout: layout.update({
                "gizclaw": {"endpoint": "missing-port"},
            }),
            "host:port",
        )

    def test_business_fixtures_are_rejected(self):
        forbidden = (
            "gizclaw",
            "contact",
            "points",
            "friend_groups",
            "friends",
            "social_invite_token",
            "profile_name_extraction",
            "pairing",
            "update",
            "pet",
            "workflows",
            "conversation_reply",
        )
        for field in forbidden:
            with self.subTest(field=field):
                self.assert_invalid(
                    lambda value, name=field: value.update({
                        "simulation": {name: None},
                    }),
                    field,
                )

    def test_json_integer_fixtures_stay_exact_in_binary64_range(self):
        maximum = layout_module.JSON_EXACT_INTEGER_MAX
        value = copy.deepcopy(self.valid)
        value["simulation"] = {
            "telemetry": {
                "hardware_version": "desktop",
                "free_memory_bytes": maximum,
            },
        }
        validated = self.validate(value)
        self.assertEqual(
            maximum,
            validated["simulation"]["telemetry"]["free_memory_bytes"],
        )
        self.assert_invalid(
            lambda layout: layout.update({
                "simulation": {
                    "telemetry": {
                        "hardware_version": "desktop",
                        "free_memory_bytes": maximum + 1,
                    },
                },
            }),
            "free_memory_bytes",
        )

    def test_duplicate_identity_and_key_fail(self):
        self.assert_invalid(
            lambda value: value["peripherals"].append({
                "periph_id": 1,
                "kind": {"imu": {"simulate": True}},
            }),
            "duplicate peripheral",
        )
        self.assert_invalid(
            lambda value: value["peripherals"].append({
                "periph_id": 3,
                "kind": {"radio_button": {"key": "space", "group_id": 9}},
            }),
            "duplicate button key",
        )

    def test_mount_traversal_overlap_missing_and_symlink_fail(self):
        self.assert_invalid(
            lambda value: value["filesystem"]["mounts"][0].update(
                {"source": "../data"}),
            "source",
        )
        self.assert_invalid(
            lambda value: value["filesystem"]["mounts"].append(
                {"source": "data", "target": "/data/cache"}),
            "overlaps",
        )
        self.assert_invalid(
            lambda value: value["filesystem"]["mounts"][0].update(
                {"source": "missing"}),
            "existing directory",
        )
        (self.root / "data-link").symlink_to(self.root / "data",
                                             target_is_directory=True)
        self.assert_invalid(
            lambda value: value["filesystem"]["mounts"][0].update(
                {"source": "data-link"}),
            "symlink",
        )
        (self.root / "parent-link").symlink_to(self.root,
                                               target_is_directory=True)
        self.assert_invalid(
            lambda value: value["filesystem"]["mounts"][0].update(
                {"source": "parent-link/data"}),
            "symlink",
        )

    def test_duplicate_json_fields_fail_closed(self):
        path = self.root / "duplicate.json"
        path.write_text(
            '{"display":{"title":"one","title":"two"}}',
            encoding="utf-8",
        )
        with self.assertRaisesRegex(layout_module.LayoutError,
                                    "duplicate JSON field 'title'"):
            layout_module.load_json(path)

    def test_unsupported_external_peripheral_fails(self):
        self.assert_invalid(
            lambda value: value["peripherals"][1]["kind"]["nfc_reader"].update(
                {"simulate": False}),
            "external NFC",
        )


if __name__ == "__main__":
    unittest.main()
