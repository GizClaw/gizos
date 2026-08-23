#!/usr/bin/env python3
"""Validate a Desktop layout JSON file and emit its native C++ configuration."""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import re
import sys
from typing import Any, NoReturn


KEYS = (
    "space", "enter", "escape", "tab", "backspace", "up", "down", "left",
    "right", "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k",
    "l", "m", "n", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x",
    "y", "z", "digit_0", "digit_1", "digit_2", "digit_3", "digit_4",
    "digit_5", "digit_6", "digit_7", "digit_8", "digit_9",
)
KEY_VALUES = {name: index + 1 for index, name in enumerate(KEYS)}
PERIPHERAL_KIND_VALUES = {
    "button": 1,
    "nfc_reader": 2,
    "imu": 3,
    "battery": 4,
    "pwm_switch": 5,
    "gpio_irq": 6,
    "radio_button": 7,
}
WIFI_SECURITY = (
    "open", "wep", "wpa", "wpa2", "wpa3", "wpa_wpa2", "wpa2_wpa3",
    "enterprise",
)
WIFI_SCAN_OUTCOME = ("success", "io_error", "timeout")
MODEM_RAT = (
    "unknown", "gsm", "gprs", "edge", "wcdma", "hspa", "lte", "lte_m",
    "nb_iot", "nr5g",
)
BATTERY_FLAGS = {
    "absent": 0,
    "discharging": 1 | 2 | 8,
    "charging": 1 | 2 | 8 | 16,
    "full": 1 | 2 | 8 | 32,
}
JSON_EXACT_INTEGER_MAX = 2**53 - 1


class LayoutError(ValueError):
    """A diagnostic tied to one JSON field."""


def fail(path: str, message: str) -> NoReturn:
    raise LayoutError(f"{path}: {message}")


def obj(value: Any, path: str, allowed: set[str],
        required: set[str] = frozenset()) -> dict[str, Any]:
    if not isinstance(value, dict):
        fail(path, "expected object")
    unknown = sorted(set(value) - allowed)
    if unknown:
        fail(path, f"unknown field '{unknown[0]}'")
    missing = sorted(required - set(value))
    if missing:
        fail(path, f"missing required field '{missing[0]}'")
    return value


def array(value: Any, path: str, maximum: int | None = None) -> list[Any]:
    if not isinstance(value, list):
        fail(path, "expected array")
    if maximum is not None and len(value) > maximum:
        fail(path, f"contains more than {maximum} items")
    return value


def boolean(value: Any, path: str) -> bool:
    if not isinstance(value, bool):
        fail(path, "expected boolean")
    return value


def integer(value: Any, path: str, minimum: int, maximum: int) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        fail(path, "expected integer")
    if value < minimum or value > maximum:
        fail(path, f"must be in [{minimum}, {maximum}]")
    return value


def text(value: Any, path: str, *, minimum: int = 0,
         maximum: int | None = None, no_nul: bool = False) -> str:
    if not isinstance(value, str):
        fail(path, "expected string")
    try:
        encoded = value.encode("utf-8", errors="strict")
    except UnicodeEncodeError:
        fail(path, "must contain valid UTF-8")
    if len(encoded) < minimum:
        fail(path, f"must contain at least {minimum} UTF-8 byte(s)")
    if maximum is not None and len(encoded) > maximum:
        fail(path, f"must contain at most {maximum} UTF-8 byte(s)")
    if no_nul and "\0" in value:
        fail(path, "must not contain an embedded NUL")
    return value


def enum(value: Any, path: str, values: tuple[str, ...]) -> str:
    result = text(value, path)
    if result not in values:
        fail(path, f"unsupported value '{result}'")
    return result


def relative_path(value: Any, path: str) -> str:
    result = text(value, path, minimum=1, no_nul=True)
    if os.path.isabs(result):
        fail(path, "must be repository-relative")
    parts = result.split("/")
    if any(part in ("", ".", "..") for part in parts):
        fail(path, "must not contain empty, '.' or '..' components")
    return result


def portable_path(value: Any, path: str) -> str:
    result = text(value, path, minimum=2, no_nul=True)
    if not result.startswith("/") or result.endswith("/"):
        fail(path, "must be an absolute portable path without trailing slash")
    if any(part in ("", ".", "..") for part in result[1:].split("/")):
        fail(path, "must not contain empty, '.' or '..' components")
    return result


def validate_layout(layout: Any, app_name: str, repo_root: pathlib.Path) -> dict[str, Any]:
    root = obj(
        layout,
        "$",
        {"display", "filesystem", "peripherals", "simulation", "gizclaw"},
        {"display", "filesystem", "peripherals"},
    )

    display = obj(root["display"], "$.display", {"title", "width", "height"},
                  {"title", "width", "height"})
    text(display["title"], "$.display.title", minimum=1, no_nul=True)
    integer(display["width"], "$.display.width", 1, 2**31 - 1)
    integer(display["height"], "$.display.height", 1, 2**31 - 1)

    filesystem = obj(root["filesystem"], "$.filesystem", {"mounts"}, {"mounts"})
    mounts = array(filesystem["mounts"], "$.filesystem.mounts")
    targets: list[str] = []
    for index, raw_mount in enumerate(mounts):
        path = f"$.filesystem.mounts[{index}]"
        mount = obj(raw_mount, path, {"source", "target"}, {"source", "target"})
        source = relative_path(mount["source"], f"{path}.source")
        target = portable_path(mount["target"], f"{path}.target")
        for previous in targets:
            if (target == previous or target.startswith(previous + "/") or
                    previous.startswith(target + "/")):
                fail(f"{path}.target", f"overlaps '{previous}'")
        targets.append(target)
        source_path = repo_root.joinpath(*source.split("/"))
        try:
            current = repo_root
            for component in source.split("/"):
                current = current / component
                if current.is_symlink():
                    fail(f"{path}.source", "must not contain a symlink")
            if not source_path.is_dir():
                fail(f"{path}.source", "does not name an existing directory")
        except OSError as error:
            fail(f"{path}.source", f"cannot inspect source: {error}")

    peripherals = array(root["peripherals"], "$.peripherals")
    peripheral_ids: set[int] = set()
    button_keys: set[str] = set()
    for index, raw_peripheral in enumerate(peripherals):
        path = f"$.peripherals[{index}]"
        peripheral = obj(raw_peripheral, path, {"periph_id", "kind"},
                         {"periph_id", "kind"})
        peripheral_id = integer(peripheral["periph_id"], f"{path}.periph_id",
                                1, 2**32 - 1)
        if peripheral_id in peripheral_ids:
            fail(f"{path}.periph_id", "duplicate peripheral id")
        peripheral_ids.add(peripheral_id)
        kind_object = obj(peripheral["kind"], f"{path}.kind",
                          set(PERIPHERAL_KIND_VALUES))
        if len(kind_object) != 1:
            fail(f"{path}.kind", "must contain exactly one peripheral kind")
        kind, raw_config = next(iter(kind_object.items()))
        config_path = f"{path}.kind.{kind}"
        if kind == "button":
            config = obj(raw_config, config_path, {"key"}, {"key"})
            key = enum(config["key"], f"{config_path}.key", KEYS)
            if key in button_keys:
                fail(f"{config_path}.key", "duplicate button key")
            button_keys.add(key)
        elif kind == "radio_button":
            config = obj(raw_config, config_path, {"key", "group_id"},
                         {"key", "group_id"})
            key = enum(config["key"], f"{config_path}.key", KEYS)
            if key in button_keys:
                fail(f"{config_path}.key", "duplicate button key")
            button_keys.add(key)
            integer(config["group_id"], f"{config_path}.group_id", 1, 2**32 - 1)
        elif kind == "nfc_reader":
            config = obj(raw_config, config_path, {"simulate", "key"},
                         {"simulate"})
            if not boolean(config["simulate"], f"{config_path}.simulate"):
                fail(f"{config_path}.simulate", "external NFC is unsupported")
            if config.get("key") is not None:
                enum(config["key"], f"{config_path}.key", KEYS)
        elif kind in ("imu", "gpio_irq"):
            config = obj(raw_config, config_path, {"simulate"}, {"simulate"})
            if not boolean(config["simulate"], f"{config_path}.simulate"):
                fail(f"{config_path}.simulate", "external peripheral is unsupported")
        elif kind == "battery":
            config = obj(raw_config, config_path,
                         {"voltage_mv", "percent_x100", "state"},
                         {"voltage_mv", "percent_x100", "state"})
            integer(config["voltage_mv"], f"{config_path}.voltage_mv",
                    -(2**31), 2**31 - 1)
            integer(config["percent_x100"], f"{config_path}.percent_x100",
                    0, 10000)
            enum(config["state"], f"{config_path}.state",
                 tuple(BATTERY_FLAGS))
        elif kind == "pwm_switch":
            config = obj(raw_config, config_path, {"duty_x100"}, {"duty_x100"})
            integer(config["duty_x100"], f"{config_path}.duty_x100", 0, 10000)

    gizclaw = root.setdefault("gizclaw", None)
    if gizclaw is not None:
        gizclaw = obj(gizclaw, "$.gizclaw", {"endpoint"}, {"endpoint"})
        endpoint = text(gizclaw["endpoint"], "$.gizclaw.endpoint",
                        minimum=1, no_nul=True)
        if ":" not in endpoint:
            fail("$.gizclaw.endpoint", "must include host:port")

    simulation = root.setdefault("simulation", {})
    simulation = obj(
        simulation, "$.simulation",
        {"wifi_sta", "modem", "telemetry"},
    )
    wifi = simulation.setdefault("wifi_sta", {})
    wifi = obj(
        wifi, "$.simulation.wifi_sta",
        {
            "connected", "saved", "ssid", "rssi_dbm", "channel",
            "scan_outcome", "scan_delay_ms", "scan_results",
        },
    )
    defaults = {
        "connected": False, "saved": False, "ssid": "", "rssi_dbm": -42,
        "channel": 1, "scan_outcome": "success", "scan_delay_ms": 0,
        "scan_results": None,
    }
    for field, default in defaults.items():
        wifi.setdefault(field, default)
    connected = boolean(wifi["connected"], "$.simulation.wifi_sta.connected")
    saved = boolean(wifi["saved"], "$.simulation.wifi_sta.saved")
    ssid = text(wifi["ssid"], "$.simulation.wifi_sta.ssid",
                maximum=32, no_nul=True)
    if connected and not ssid:
        fail("$.simulation.wifi_sta.ssid", "connected Wi-Fi requires an SSID")
    integer(wifi["rssi_dbm"], "$.simulation.wifi_sta.rssi_dbm", -32768, 32767)
    integer(wifi["channel"], "$.simulation.wifi_sta.channel", 1, 14)
    enum(wifi["scan_outcome"], "$.simulation.wifi_sta.scan_outcome",
         WIFI_SCAN_OUTCOME)
    integer(wifi["scan_delay_ms"], "$.simulation.wifi_sta.scan_delay_ms",
            0, 60000)
    scan_results = [] if wifi["scan_results"] is None else array(
        wifi["scan_results"], "$.simulation.wifi_sta.scan_results", 16)
    saved_found = False
    for index, raw_entry in enumerate(scan_results):
        path = f"$.simulation.wifi_sta.scan_results[{index}]"
        entry = obj(raw_entry, path,
                    {"ssid", "rssi_dbm", "channel", "security", "password"},
                    {"ssid"})
        entry.setdefault("rssi_dbm", -60)
        entry.setdefault("channel", 1)
        entry.setdefault("security", "wpa2")
        entry.setdefault("password", "")
        entry_ssid = text(entry["ssid"], f"{path}.ssid", minimum=1,
                          maximum=32, no_nul=True)
        integer(entry["rssi_dbm"], f"{path}.rssi_dbm", -32768, 32767)
        integer(entry["channel"], f"{path}.channel", 1, 14)
        security = enum(entry["security"], f"{path}.security", WIFI_SECURITY)
        password = text(entry["password"], f"{path}.password", maximum=64,
                        no_nul=True)
        if (security == "open" and password) or \
                (security != "open" and not password):
            fail(f"{path}.password", "does not match Wi-Fi security")
        saved_found = saved_found or entry_ssid == ssid
    if saved and (not connected or not saved_found):
        fail("$.simulation.wifi_sta.saved",
             "saved Wi-Fi must be connected and present in scan results")

    modem = simulation.setdefault("modem", {})
    modem = obj(modem, "$.simulation.modem",
                {"available", "mobile_data_enabled", "operator_name",
                 "rssi_dbm", "rat"})
    for field, default in {
        "available": False, "mobile_data_enabled": False,
        "operator_name": None, "rssi_dbm": -85, "rat": "unknown",
    }.items():
        modem.setdefault(field, default)
    available = boolean(modem["available"], "$.simulation.modem.available")
    mobile = boolean(modem["mobile_data_enabled"],
                     "$.simulation.modem.mobile_data_enabled")
    if modem["operator_name"] is not None:
        text(modem["operator_name"], "$.simulation.modem.operator_name",
             maximum=31, no_nul=True)
    integer(modem["rssi_dbm"], "$.simulation.modem.rssi_dbm", -32768, 32767)
    enum(modem["rat"], "$.simulation.modem.rat", MODEM_RAT)
    if mobile and not available:
        fail("$.simulation.modem.mobile_data_enabled",
             "mobile data requires an available modem")

    telemetry = simulation.setdefault("telemetry", None)
    if telemetry is not None:
        telemetry = obj(
            telemetry, "$.simulation.telemetry",
            {"hardware_version", "gnss", "free_memory_bytes",
             "temperature_milli_celsius"},
            {"hardware_version"},
        )
        telemetry.setdefault("gnss", None)
        telemetry.setdefault("free_memory_bytes", None)
        telemetry.setdefault("temperature_milli_celsius", None)
        text(telemetry["hardware_version"],
             "$.simulation.telemetry.hardware_version", minimum=1,
             maximum=96, no_nul=True)
        if telemetry["gnss"] is not None:
            gnss = obj(telemetry["gnss"], "$.simulation.telemetry.gnss",
                       {"latitude_e7", "longitude_e7", "altitude_cm"},
                       {"latitude_e7", "longitude_e7"})
            gnss.setdefault("altitude_cm", 0)
            integer(gnss["latitude_e7"],
                    "$.simulation.telemetry.gnss.latitude_e7",
                    -900_000_000, 900_000_000)
            integer(gnss["longitude_e7"],
                    "$.simulation.telemetry.gnss.longitude_e7",
                    -1_800_000_000, 1_800_000_000)
            integer(gnss["altitude_cm"],
                    "$.simulation.telemetry.gnss.altitude_cm",
                    -(2**31), 2**31 - 1)
        if telemetry["free_memory_bytes"] is not None:
            integer(telemetry["free_memory_bytes"],
                    "$.simulation.telemetry.free_memory_bytes", 0,
                    JSON_EXACT_INTEGER_MAX)
        if telemetry["temperature_milli_celsius"] is not None:
            integer(telemetry["temperature_milli_celsius"],
                    "$.simulation.telemetry.temperature_milli_celsius",
                    -100_000, 200_000)

    if app_name == "example/display" and mounts:
        fail("$.filesystem.mounts", "example/display cannot declare mounts")
    if app_name == "example/audio-system":
        if len(mounts) != 1 or \
                mounts[0]["source"] != "projects/example/apps/audio-system/data" or \
                mounts[0]["target"] != "/data":
            fail("$.filesystem.mounts",
                 "example/audio-system requires its /data audio mount")
    relative_path(app_name, "app_name")
    return root


def c_string(value: str) -> str:
    # JSON string escaping is also valid for a UTF-8 C++ source literal for
    # every value accepted by text(): quotes, backslashes, and control bytes
    # are escaped while non-ASCII text remains unambiguous UTF-8.  Avoid
    # hexadecimal escapes here because C++ greedily consumes following hex
    # digits and can silently change adjacent UTF-8 bytes.
    return json.dumps(value, ensure_ascii=False)


def peripheral_initializer(peripheral: dict[str, Any]) -> str:
    kind, config = next(iter(peripheral["kind"].items()))
    key = config.get("key")
    battery_flags = BATTERY_FLAGS.get(config.get("state", "absent"), 0)
    return (
        "  {"
        f"{peripheral['periph_id']}u, "
        f"static_cast<h2_desktop_peripheral_kind_t>({PERIPHERAL_KIND_VALUES[kind]}), "
        f"static_cast<h2_desktop_key_t>({KEY_VALUES.get(key, 0)}), "
        f"{str(config.get('simulate', True)).lower()}, "
        f"{config.get('group_id', 0)}u, "
        f"{config.get('voltage_mv', 0)}, "
        f"{battery_flags}u, "
        f"{config.get('percent_x100', 0)}u, "
        f"{config.get('duty_x100', 0)}u"
        "},"
    )


def emit_header(layout: dict[str, Any], app_name: str) -> str:
    normalized = json.dumps(layout, ensure_ascii=False, sort_keys=True,
                            separators=(",", ":"))
    if ")H2LAYOUT\"" in normalized:
        fail("$", "layout contains reserved raw-string delimiter")
    mounts = layout["filesystem"]["mounts"]
    peripherals = layout["peripherals"]
    lines = [
        "#ifndef H2_DESKTOP_GENERATED_LAYOUT_CONFIG_H",
        "#define H2_DESKTOP_GENERATED_LAYOUT_CONFIG_H",
        "",
        '#include "h2_desktop_app_support.h"',
        "",
        "#include <cstddef>",
        "",
        "namespace h2_desktop_layout {",
        f"inline constexpr char app_name[] = {c_string(app_name)};",
        f"inline constexpr char title[] = {c_string(layout['display']['title'])};",
        f"inline constexpr int width = {layout['display']['width']};",
        f"inline constexpr int height = {layout['display']['height']};",
        f"inline constexpr char normalized_json[] = R\"H2LAYOUT({normalized})H2LAYOUT\";",
        "",
    ]
    if mounts:
        lines.append("inline constexpr h2::desktop::FilesystemMount mounts[] = {")
        lines.extend(
            f"  {{{c_string(mount['source'])}, {c_string(mount['target'])}}},"
            for mount in mounts
        )
        lines.append("};")
        lines.append(
            "inline constexpr std::size_t mount_count = "
            "sizeof(mounts) / sizeof(mounts[0]);"
        )
    else:
        lines.extend([
            "inline constexpr const h2::desktop::FilesystemMount *mounts = nullptr;",
            "inline constexpr std::size_t mount_count = 0u;",
        ])
    lines.append("")
    if peripherals:
        lines.append(
            "inline constexpr h2_desktop_peripheral_config_t peripherals[] = {")
        lines.extend(peripheral_initializer(item) for item in peripherals)
        lines.append("};")
        lines.append(
            "inline constexpr std::size_t peripheral_count = "
            "sizeof(peripherals) / sizeof(peripherals[0]);"
        )
    else:
        lines.extend([
            "inline constexpr const h2_desktop_peripheral_config_t "
            "*peripherals = nullptr;",
            "inline constexpr std::size_t peripheral_count = 0u;",
        ])
    lines.extend([
        "",
        "}  // namespace h2_desktop_layout",
        "",
        "#endif  // H2_DESKTOP_GENERATED_LAYOUT_CONFIG_H",
        "",
    ])
    return "\n".join(lines)


def load_json(path: pathlib.Path) -> Any:
    try:
        with path.open("r", encoding="utf-8", errors="strict") as stream:
            return json.load(
                stream,
                object_pairs_hook=lambda pairs: _unique_object(path, pairs),
            )
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise LayoutError(f"{path}: {error}") from error


def _unique_object(path: pathlib.Path,
                   pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise LayoutError(f"{path}: duplicate JSON field '{key}'")
        result[key] = value
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument("--app-name", required=True)
    parser.add_argument("--repo-root", default=".", type=pathlib.Path)
    args = parser.parse_args()
    try:
        layout = validate_layout(load_json(args.input), args.app_name,
                                 args.repo_root.resolve())
        output = emit_header(layout, args.app_name)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(output, encoding="utf-8", newline="\n")
    except LayoutError as error:
        print(f"Desktop layout validation failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
