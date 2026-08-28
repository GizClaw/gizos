import os
from pathlib import Path
import unittest


class BleAdvertisingContractTest(unittest.TestCase):
    def test_default_app_management_service_uses_legacy_advertising(self):
        runfiles = Path(os.environ["TEST_SRCDIR"])
        sources = list(runfiles.rglob("h2_bk_h2loader_app_bleikcp.c"))

        self.assertEqual(1, len(sources), [str(path) for path in sources])
        source = sources[0].read_text(encoding="utf-8")
        default_start = source.index("int h2_bk_h2loader_start_app_ble(")
        default_with_capabilities = source.index(
            "int h2_bk_h2loader_start_app_ble_with_capabilities("
        )
        explicit_extended = source.index(
            "int h2_bk_h2loader_start_app_ble_extended("
        )

        default_body = source[default_start:default_with_capabilities]
        capabilities_body = source[
            default_with_capabilities:explicit_extended
        ]
        self.assertIn("H2_LOADER_BLE_ADVERTISING_LEGACY", default_body)
        self.assertNotIn("H2_LOADER_BLE_ADVERTISING_EXTENDED", default_body)
        self.assertIn(
            "H2_LOADER_BLE_ADVERTISING_LEGACY", capabilities_body
        )
        self.assertNotIn(
            "H2_LOADER_BLE_ADVERTISING_EXTENDED", capabilities_body
        )

    def test_identity_uses_legacy_sized_manufacturer_data(self):
        runfiles = Path(os.environ["TEST_SRCDIR"])
        sources = list(runfiles.rglob("h2_loader_ble.c"))

        self.assertEqual(1, len(sources), [str(path) for path in sources])
        source = sources[0].read_text(encoding="utf-8")
        advertising = source.index(
            "const h2_pal_ble_adv_data_t adv_data = {"
        )
        update = source.index("int rc = stop_first", advertising)
        advertising_block = source[advertising:update]

        self.assertIn(".manufacturer_data = {", advertising_block)
        self.assertIn(".service_data = { 0 }", advertising_block)

    def test_host_accepts_identity_from_manufacturer_data(self):
        runfiles = Path(os.environ["TEST_SRCDIR"])
        sources = list(runfiles.rglob("h2_h2loader_host_discovery.c"))

        self.assertEqual(1, len(sources), [str(path) for path in sources])
        source = sources[0].read_text(encoding="utf-8")
        parser = source.index("static int parse_ble_identity(")
        callback = source.index("static bool scan_ble_callback(")
        parser_body = source[parser:callback]

        self.assertIn("result->service_data.len > 0u", parser_body)
        self.assertIn("result->manufacturer_data.data", parser_body)
        self.assertIn("result->manufacturer_data.len", parser_body)

    def test_ethermind_uses_gatts_connection_identifiers(self):
        runfiles = Path(os.environ["TEST_SRCDIR"])
        sources = list(runfiles.rglob("h2_bk_platform_ble.c"))

        self.assertEqual(1, len(sources), [str(path) for path in sources])
        source = sources[0].read_text(encoding="utf-8")
        gatts_connect = source.index("case BK_GATTS_CONNECT_EVT:")
        gatts_disconnect = source.index("case BK_GATTS_DISCONNECT_EVT:")
        gatts_default = source.index("    default:", gatts_disconnect)
        gatts_connect_body = source[gatts_connect:gatts_disconnect]
        gatts_disconnect_body = source[gatts_disconnect:gatts_default]
        self.assertNotIn("link_role", gatts_connect_body)
        self.assertIn(
            "connection.conn_handle = param->connect.conn_id",
            gatts_connect_body,
        )
        self.assertIn(
            "connection.mtu = H2_BK_BLE_LOCAL_MAX_MTU",
            gatts_connect_body,
        )
        self.assertIn(
            "h2_bk_ble_mark_connectable_advertising_stopped()",
            gatts_connect_body,
        )
        self.assertIn(
            "h2_bk_ble_mark_connectable_advertising_stopped()",
            gatts_disconnect_body,
        )
        helper = source.index(
            "static void h2_bk_ble_mark_connectable_advertising_stopped("
        )
        helper_end = source.index(
            "static int h2_bk_ble_adv_set_valid(", helper
        )
        helper_body = source[helper:helper_end]
        self.assertIn("set->active = 0", helper_body)
        self.assertIn(
            "H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STOPPED",
            helper_body,
        )
        set_phy = source.index("static h2_pal_result_t h2_bk_ble_set_preferred_phy(")
        read_phy = source.index("static h2_pal_result_t h2_bk_ble_read_phy(")
        self.assertIn(
            "conn_handle == s_h2_bk_ble_peripheral_conn_handle",
            source[set_phy:read_phy],
        )
        exchange_mtu = source.index("static h2_pal_result_t h2_bk_ble_exchange_mtu(")
        update_connection = source.index(
            "static h2_pal_result_t h2_bk_ble_update_connection("
        )
        self.assertIn(
            "conn_handle == s_h2_bk_ble_peripheral_conn_handle",
            source[exchange_mtu:set_phy],
        )
        self.assertIn(
            "conn_handle == s_h2_bk_ble_peripheral_conn_handle",
            source[update_connection:exchange_mtu],
        )

        legacy_connect = source.index("} else if (notice == BLE_5_CONNECT_EVENT)")
        legacy_disconnect = source.index(
            "} else if (notice == BLE_5_DISCONNECT_EVENT)"
        )
        legacy_init_connect = source.index(
            "} else if (notice == BLE_5_INIT_CONNECT_EVENT)"
        )
        self.assertIn(
            "h2_bk_ble_mark_connectable_advertising_stopped()",
            source[legacy_connect:legacy_disconnect],
        )
        self.assertIn(
            "h2_bk_ble_mark_connectable_advertising_stopped()",
            source[legacy_disconnect:legacy_init_connect],
        )

    def test_ethermind_rx_does_not_reenter_attribute_access(self):
        runfiles = Path(os.environ["TEST_SRCDIR"])
        sources = list(runfiles.rglob("h2_bk_platform_ble.c"))

        self.assertEqual(1, len(sources), [str(path) for path in sources])
        source = sources[0].read_text(encoding="utf-8")
        write_event = source.index("case BK_GATTS_WRITE_EVT:")
        connect_event = source.index("case BK_GATTS_CONNECT_EVT:")
        write_body = source[write_event:connect_event]

        self.assertNotIn("bk_ble_gatts_get_attr_value(", write_body)

    def test_ethermind_notifications_do_not_wait_for_confirmation(self):
        runfiles = Path(os.environ["TEST_SRCDIR"])
        sources = list(runfiles.rglob("h2_bk_platform_ble.c"))

        self.assertEqual(1, len(sources), [str(path) for path in sources])
        source = sources[0].read_text(encoding="utf-8")
        notify = source.index("static h2_pal_result_t h2_bk_ble_notify(")
        indicate = source.index("static h2_pal_result_t h2_bk_ble_indicate(")
        notify_body = source[notify:indicate]

        self.assertIn("bk_ble_gatts_send_indicate(", notify_body)
        self.assertNotIn("h2_bk_ble_wait_notify(", notify_body)

    def test_loader_ble_memory_budget_fits_display_layout(self):
        runfiles = Path(os.environ["TEST_SRCDIR"])
        sources = list(runfiles.rglob("h2_loader_ble.c"))

        self.assertEqual(1, len(sources), [str(path) for path in sources])
        source = sources[0].read_text(encoding="utf-8")
        self.assertIn("H2_LOADER_BLE_INPUT_FRAME_CAPACITY 16u", source)
        self.assertIn("H2_LOADER_BLE_BUFFER_SIZE (8u * 1024u)", source)

if __name__ == "__main__":
    unittest.main()
