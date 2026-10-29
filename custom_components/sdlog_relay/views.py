"""HTTP views. Each one only resolves who is asking and delegates to relay_core."""

from __future__ import annotations

from typing import TYPE_CHECKING

from aiohttp import web

from homeassistant.components.http import KEY_HASS_USER, HomeAssistantView

from .relay_core import serve_firmware, upload_firmware

if TYPE_CHECKING:
    from . import RelayData


class PassThroughView(HomeAssistantView):
    """The four collection endpoints, any authenticated user (R1, R7)."""

    url = "/api/sdlog_relay/{logger_id}/sdlog/{tail:.+}"
    name = "api:sdlog_relay:sdlog"
    requires_auth = True

    def __init__(self, data: RelayData) -> None:
        self._data = data

    async def get(self, request: web.Request, logger_id: str, tail: str) -> web.StreamResponse:
        return await self._data.relay.forward(request, self._data.targets, logger_id, tail)

    async def post(self, request: web.Request, logger_id: str, tail: str) -> web.StreamResponse:
        return await self._data.relay.forward(request, self._data.targets, logger_id, tail)


class FirmwareView(HomeAssistantView):
    """No HA auth; relay_core answers only the logger's own address (R9)."""

    url = "/api/sdlog_relay/{logger_id}/fw/{name}"
    name = "api:sdlog_relay:fw"
    requires_auth = False

    def __init__(self, data: RelayData) -> None:
        self._data = data

    async def get(self, request: web.Request, logger_id: str, name: str) -> web.StreamResponse:
        return await serve_firmware(request, self._data.store, self._data.targets, logger_id, name)


class FirmwareUploadView(HomeAssistantView):
    """Admin only (R10)."""

    url = "/api/sdlog_relay/{logger_id}/fw"
    name = "api:sdlog_relay:fw_upload"
    requires_auth = True

    def __init__(self, data: RelayData) -> None:
        self._data = data

    async def put(self, request: web.Request, logger_id: str) -> web.Response:
        user = request[KEY_HASS_USER]
        return await upload_firmware(
            request, self._data.store, self._data.targets, logger_id, is_admin=user.is_admin
        )
