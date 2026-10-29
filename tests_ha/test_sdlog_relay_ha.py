"""sdlog_relay inside a real Home Assistant: auth, admin check, XFF, ip_ban, config flow.

The wire behaviour is proven in tests/relay/ without HA. These cases prove what
only HA decides: who is authenticated, who is admin, what ``request.remote`` is
after ``X-Forwarded-For``, and what counts as a failed login.
"""

from __future__ import annotations

import hashlib
import importlib.util
from ipaddress import ip_address
from pathlib import Path
import sys

import pytest

from homeassistant.components.http.ban import KEY_FAILED_LOGIN_ATTEMPTS
from homeassistant.config_entries import SOURCE_USER
from homeassistant.const import CONF_HOST, CONF_PORT
from homeassistant.core import HomeAssistant
from homeassistant.data_entry_flow import FlowResultType
from homeassistant.setup import async_setup_component

from pytest_homeassistant_custom_component.common import MockConfigEntry

from custom_components.sdlog_relay.const import CONF_LOGGER_ID, DOMAIN

_COLLECT = Path(__file__).parent.parent / "tests" / "sd_logger" / "test_collect.py"


def _fake_device_class():
    spec = importlib.util.spec_from_file_location("relay_ha_test_collect", _COLLECT)
    module = importlib.util.module_from_spec(spec)
    sys.modules["relay_ha_test_collect"] = module
    spec.loader.exec_module(module)
    return module.FakeDevice


FakeDevice = _fake_device_class()

# The test client talks from 127.0.0.1; HA treats it as the trusted proxy.
HTTP_CONFIG = {
    "http": {
        "use_x_forwarded_for": True,
        "trusted_proxies": ["127.0.0.1"],
        "ip_ban_enabled": True,
        "login_attempts_threshold": 5,
    }
}


@pytest.fixture
def device():
    fake = FakeDevice("mr-orange").start()
    yield fake
    fake.stop()


async def setup_relay(hass: HomeAssistant, tmp_path: Path, entries: dict[str, tuple[str, int]]) -> None:
    hass.config.config_dir = str(tmp_path)
    assert await async_setup_component(hass, "http", HTTP_CONFIG)
    for logger_id, (host, port) in entries.items():
        MockConfigEntry(
            domain=DOMAIN,
            unique_id=logger_id,
            data={CONF_LOGGER_ID: logger_id, CONF_HOST: host, CONF_PORT: port},
        ).add_to_hass(hass)
    assert await async_setup_component(hass, DOMAIN, {})
    await hass.async_block_till_done()


def failed_logins(hass: HomeAssistant) -> dict:
    return dict(hass.http.app[KEY_FAILED_LOGIN_ATTEMPTS])


def device_target(device) -> tuple[str, int]:
    host, port = device.url.removeprefix("http://").split(":")
    return host, int(port)


# ------------------------------------------------------------------ R7, R8


async def test_relay_requires_auth(hass, hass_client, hass_client_no_auth, device, tmp_path) -> None:
    device.add(431)
    await setup_relay(hass, tmp_path, {"car": device_target(device)})

    anonymous = await hass_client_no_auth()
    response = await anonymous.get("/api/sdlog_relay/car/sdlog/index")
    assert response.status == 401
    assert device.requests == []

    client = await hass_client()
    response = await client.get("/api/sdlog_relay/car/sdlog/index")
    assert response.status == 200
    assert (await response.json())["device"] == "mr-orange"


async def test_relay_any_user_may_pull(hass, hass_client, hass_read_only_access_token, device, tmp_path) -> None:
    name = device.add(431)
    await setup_relay(hass, tmp_path, {"car": device_target(device)})
    client = await hass_client(hass_read_only_access_token)

    response = await client.get(f"/api/sdlog_relay/car/sdlog/f/{name}", headers={"Range": "bytes=0-9"})
    assert response.status == 206
    assert await response.read() == device.chunks[name][:10]

    response = await client.post(f"/api/sdlog_relay/car/sdlog/done/{name}")
    assert response.status == 200
    assert device.confirmed == [name]


async def test_only_ha_auth_counts_a_failed_login(hass, hass_client, hass_client_no_auth, device, tmp_path) -> None:
    """One 401 from HA's auth layer is one failed login. Nothing the relay
    answers on its own (404, 400, 502, the firmware 404) is."""
    await setup_relay(hass, tmp_path, {"car": device_target(device), "other": ("192.0.2.10", 8080)})
    client = await hass_client()
    anonymous = await hass_client_no_auth()

    for path in (
        "/api/sdlog_relay/nope/sdlog/index",
        "/api/sdlog_relay/car/sdlog/f/..",
        "/api/sdlog_relay/car/sdlog/config",
    ):
        assert (await client.get(path)).status in (400, 404)
    for _ in range(6):
        assert (await anonymous.get("/api/sdlog_relay/other/fw/manifest.json")).status == 404
    assert failed_logins(hass) == {}

    assert (await anonymous.get("/api/sdlog_relay/car/sdlog/index")).status == 401
    assert failed_logins(hass) == {ip_address("127.0.0.1"): 1}


# --------------------------------------------------------------- R9, R10


async def upload(client, data: bytes, logger: str = "car"):
    return await client.put(
        f"/api/sdlog_relay/{logger}/fw", params={"version": "1.2.3", "chip_family": "ESP32-C6"}, data=data
    )


async def test_fw_upload_admin_only(hass, hass_client, hass_read_only_access_token, tmp_path) -> None:
    await setup_relay(hass, tmp_path, {"car": ("127.0.0.1", 8080)})

    user = await hass_client(hass_read_only_access_token)
    response = await upload(user, b"image")
    assert response.status == 403
    assert not (tmp_path / "sdlog_relay").exists()
    assert failed_logins(hass) == {}

    admin = await hass_client()
    response = await upload(admin, b"image")
    assert response.status == 200
    md5 = hashlib.md5(b"image").hexdigest()  # noqa: S324
    assert (tmp_path / "sdlog_relay" / "car" / f"firmware-{md5}.ota.bin").read_bytes() == b"image"


async def test_fw_only_for_logger_ip(hass, hass_client, hass_client_no_auth, tmp_path) -> None:
    await setup_relay(hass, tmp_path, {"car": ("127.0.0.1", 8080), "other": ("192.0.2.10", 8080)})
    admin = await hass_client()
    await upload(admin, b"car-image")
    await upload(admin, b"other-image", logger="other")

    anonymous = await hass_client_no_auth()
    response = await anonymous.get("/api/sdlog_relay/car/fw/manifest.json")
    assert response.status == 200
    assert (await response.json())["builds"][0]["ota"]["md5"] == hashlib.md5(b"car-image").hexdigest()  # noqa: S324
    assert (await anonymous.get("/api/sdlog_relay/other/fw/manifest.json")).status == 404


async def test_fw_xff_from_trusted_proxy_not_spoofable(hass, hass_client, hass_client_no_auth, tmp_path) -> None:
    """Behind a trusted proxy HA takes the rightmost untrusted XFF entry. A
    client that writes the logger's address in front of its own gets nothing."""
    await setup_relay(hass, tmp_path, {"other": ("192.0.2.10", 8080)})
    await upload(await hass_client(), b"image", logger="other")
    anonymous = await hass_client_no_auth()
    url = "/api/sdlog_relay/other/fw/manifest.json"

    spoofed = await anonymous.get(url, headers={"X-Forwarded-For": "192.0.2.10, 203.0.113.9"})
    assert spoofed.status == 404
    honest = await anonymous.get(url, headers={"X-Forwarded-For": "192.0.2.10"})
    assert honest.status == 200
    assert failed_logins(hass) == {}


# ------------------------------------------------------------- config flow


async def test_flow_creates_entry_and_validates(hass: HomeAssistant, tmp_path) -> None:
    hass.config.config_dir = str(tmp_path)
    result = await hass.config_entries.flow.async_init(DOMAIN, context={"source": SOURCE_USER})
    assert result["type"] is FlowResultType.FORM

    result = await hass.config_entries.flow.async_configure(
        result["flow_id"], {CONF_LOGGER_ID: "Car 1", CONF_HOST: "logger.local", CONF_PORT: 0}
    )
    assert result["errors"] == {CONF_LOGGER_ID: "bad_logger_id", CONF_HOST: "bad_host", CONF_PORT: "bad_port"}

    result = await hass.config_entries.flow.async_configure(
        result["flow_id"], {CONF_LOGGER_ID: "car", CONF_HOST: "192.0.2.10", CONF_PORT: 8080}
    )
    assert result["type"] is FlowResultType.CREATE_ENTRY
    assert result["data"] == {CONF_LOGGER_ID: "car", CONF_HOST: "192.0.2.10", CONF_PORT: 8080}

    again = await hass.config_entries.flow.async_init(DOMAIN, context={"source": SOURCE_USER})
    again = await hass.config_entries.flow.async_configure(
        again["flow_id"], {CONF_LOGGER_ID: "car", CONF_HOST: "192.0.2.11", CONF_PORT: 8080}
    )
    assert again["type"] is FlowResultType.ABORT and again["reason"] == "already_configured"


async def test_reconfigure_moves_the_target(hass, hass_client, device, tmp_path) -> None:
    await setup_relay(hass, tmp_path, {"car": ("192.0.2.10", 8080)})
    entry = hass.config_entries.async_entries(DOMAIN)[0]
    host, port = device_target(device)

    result = await entry.start_reconfigure_flow(hass)
    result = await hass.config_entries.flow.async_configure(result["flow_id"], {CONF_HOST: host, CONF_PORT: port})
    assert result["type"] is FlowResultType.ABORT and result["reason"] == "reconfigure_successful"
    await hass.async_block_till_done()

    client = await hass_client()
    assert (await client.get("/api/sdlog_relay/car/sdlog/index")).status == 200


async def test_unloaded_entry_answers_404(hass, hass_client, device, tmp_path) -> None:
    await setup_relay(hass, tmp_path, {"car": device_target(device)})
    entry = hass.config_entries.async_entries(DOMAIN)[0]
    assert await hass.config_entries.async_unload(entry.entry_id)

    client = await hass_client()
    assert (await client.get("/api/sdlog_relay/car/sdlog/index")).status == 404
    assert device.requests == []
