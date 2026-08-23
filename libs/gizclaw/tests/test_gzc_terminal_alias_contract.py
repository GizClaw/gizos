from pathlib import Path
import sys
import unittest


if len(sys.argv) < 3:
    raise RuntimeError("expected the pinned implementation and smoke-test paths")

_IMPLEMENTATION_PATH = Path(sys.argv.pop(1))
_SMOKE_TEST_PATH = Path(sys.argv.pop(1))


def _function_body(source: str, signature: str) -> str:
    signature_offset = source.find(signature)
    if signature_offset < 0:
        raise AssertionError(f"missing function signature: {signature}")
    body_offset = source.find("{", signature_offset)
    if body_offset < 0:
        raise AssertionError(f"missing function body: {signature}")

    depth = 0
    for offset in range(body_offset, len(source)):
        if source[offset] == "{":
            depth += 1
        elif source[offset] == "}":
            depth -= 1
            if depth == 0:
                return source[body_offset + 1 : offset]
    raise AssertionError(f"unterminated function body: {signature}")


class TerminalAliasContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.implementation = _IMPLEMENTATION_PATH.read_text(encoding="utf-8")
        cls.smoke_test = _SMOKE_TEST_PATH.read_text(encoding="utf-8")
        cls.on_channel_state = _function_body(
            cls.implementation, "static void on_channel_state("
        )

    def test_terminal_callback_revokes_every_borrowed_alias(self) -> None:
        callback = self.on_channel_state
        self.assertIn(
            "state == GZC_RTC_CHANNEL_CLOSED || "
            "state == GZC_RTC_CHANNEL_ERROR",
            callback,
        )
        self.assertLess(
            callback.index("client->inbound_channels[i] = NULL;"),
            callback.index("gzc_rpc_inbound_destroy(inbound);"),
        )
        self.assertIn("client->rpc_channel = NULL;", callback)
        self.assertIn("service_channel->open = false;", callback)
        self.assertIn("service_channel->closed = true;", callback)
        self.assertIn("service_channel->close_requested = true;", callback)
        self.assertIn("service_channel->rtc = NULL;", callback)
        self.assertIn("service_channel == client->event_channel", callback)
        self.assertIn("client->packet_channel = NULL;", callback)

    def test_upstream_smoke_retains_terminal_behavior_coverage(self) -> None:
        smoke = self.smoke_test
        for state_array in ("service_terminal_states", "rpc_terminal_states"):
            array_offset = smoke.index(state_array)
            array = smoke[array_offset : smoke.index("};", array_offset)]
            self.assertIn("GZC_RTC_CHANNEL_CLOSED", array)
            self.assertIn("GZC_RTC_CHANNEL_ERROR", array)

        required_assertions = (
            "terminal service cleanup skips provider close after duplicate notification",
            "explicit service close remains exactly once before a late terminal callback",
            "active RPC terminal callback revokes its alias without provider close",
            "mandatory packet failure closes the physical Peer",
            "terminal Event callback closes the client without re-closing consumed channels",
        )
        for assertion in required_assertions:
            self.assertIn(assertion, smoke)

        self.assertIn("close_remote_rpc_with_state", smoke)
        self.assertIn("GZC_RTC_CHANNEL_ERROR", smoke)
        self.assertIn("stale_close_count == 0", smoke)


if __name__ == "__main__":
    unittest.main()
