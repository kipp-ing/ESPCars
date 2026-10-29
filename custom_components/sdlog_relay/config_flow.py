"""Config flow: one entry per logger, settable remotely over the flow API."""

from __future__ import annotations

import ipaddress
from typing import Any

import voluptuous as vol

from homeassistant.config_entries import ConfigFlow, ConfigFlowResult
from homeassistant.const import CONF_HOST, CONF_PORT

from .const import CONF_LOGGER_ID, DEFAULT_PORT, DOMAIN
from .relay_core import LOGGER_ID_RE


def _address_errors(user_input: dict[str, Any]) -> dict[str, str]:
    errors: dict[str, str] = {}
    try:
        ipaddress.IPv4Address(user_input[CONF_HOST])
    except ValueError:
        errors[CONF_HOST] = "bad_host"
    if not 1 <= user_input[CONF_PORT] <= 65535:
        errors[CONF_PORT] = "bad_port"
    return errors


class SdlogRelayConfigFlow(ConfigFlow, domain=DOMAIN):
    VERSION = 1

    async def async_step_user(self, user_input: dict[str, Any] | None = None) -> ConfigFlowResult:
        errors: dict[str, str] = {}
        if user_input is not None:
            if LOGGER_ID_RE.fullmatch(user_input[CONF_LOGGER_ID]) is None:
                errors[CONF_LOGGER_ID] = "bad_logger_id"
            errors |= _address_errors(user_input)
            if not errors:
                await self.async_set_unique_id(user_input[CONF_LOGGER_ID])
                self._abort_if_unique_id_configured()
                return self.async_create_entry(title=user_input[CONF_LOGGER_ID], data=user_input)
        schema = vol.Schema(
            {
                vol.Required(CONF_LOGGER_ID): str,
                vol.Required(CONF_HOST): str,
                vol.Required(CONF_PORT, default=DEFAULT_PORT): int,
            }
        )
        return self.async_show_form(step_id="user", data_schema=schema, errors=errors)

    async def async_step_reconfigure(self, user_input: dict[str, Any] | None = None) -> ConfigFlowResult:
        """The logger got a new address: change it without losing the ID."""
        entry = self._get_reconfigure_entry()
        errors: dict[str, str] = {}
        if user_input is not None:
            errors = _address_errors(user_input)
            if not errors:
                return self.async_update_reload_and_abort(entry, data_updates=user_input)
        schema = vol.Schema(
            {
                vol.Required(CONF_HOST, default=entry.data[CONF_HOST]): str,
                vol.Required(CONF_PORT, default=entry.data[CONF_PORT]): int,
            }
        )
        return self.async_show_form(step_id="reconfigure", data_schema=schema, errors=errors)
