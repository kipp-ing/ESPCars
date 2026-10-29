"""sdlog_relay: reach an sd_logger through this Home Assistant (docs/sdlog-relay-spec.md).

Thin glue: config entries become ``Target``s, the views hand every request to
``relay_core``. Nothing here touches the wire.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path

import aiohttp

from homeassistant.config_entries import ConfigEntry
from homeassistant.const import CONF_HOST, CONF_PORT
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers import config_validation as cv
from homeassistant.helpers.aiohttp_client import async_create_clientsession
from homeassistant.helpers.typing import ConfigType

from .const import CONF_LOGGER_ID, DOMAIN, FIRMWARE_DIR
from .relay_core import FirmwareStore, Relay, Target
from .views import FirmwareUploadView, FirmwareView, PassThroughView

CONFIG_SCHEMA = cv.config_entry_only_config_schema(DOMAIN)


@dataclass
class RelayData:
    relay: Relay
    store: FirmwareStore
    targets: dict[str, Target] = field(default_factory=dict)


async def async_setup(hass: HomeAssistant, config: ConfigType) -> bool:
    # Own session: no cookie jar, so nothing a logger sets ever travels back.
    session = async_create_clientsession(hass, cookie_jar=aiohttp.DummyCookieJar())
    data = RelayData(Relay(session), FirmwareStore(Path(hass.config.path(FIRMWARE_DIR))))
    hass.data[DOMAIN] = data
    # Views cannot be unregistered; they look the logger up per request, so an
    # unloaded entry answers 404.
    hass.http.register_view(PassThroughView(data))
    hass.http.register_view(FirmwareView(data))
    hass.http.register_view(FirmwareUploadView(data))
    return True


async def async_setup_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    data: RelayData = hass.data[DOMAIN]
    logger_id = entry.data[CONF_LOGGER_ID]
    data.targets[logger_id] = Target(entry.data[CONF_HOST], entry.data[CONF_PORT])

    @callback
    def _forget() -> None:
        # Must return None: HA schedules whatever an unload callback returns.
        data.targets.pop(logger_id, None)

    entry.async_on_unload(_forget)
    return True


async def async_unload_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    return True
