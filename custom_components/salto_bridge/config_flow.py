"""Config flow for Salto Bridge (manual, zeroconf discovery and reauth)."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

import voluptuous as vol

from homeassistant.config_entries import ConfigFlow, ConfigFlowResult
from homeassistant.const import CONF_HOST
from homeassistant.helpers.aiohttp_client import async_get_clientsession
from homeassistant.helpers.service_info.zeroconf import ZeroconfServiceInfo

from .api import SaltoApi, SaltoAuthError, SaltoError
from .const import CONF_TOKEN, DOMAIN


def _norm_mac(mac: str | None) -> str | None:
    return mac.replace(":", "").lower() if mac else None


class SaltoConfigFlow(ConfigFlow, domain=DOMAIN):
    VERSION = 1

    def __init__(self) -> None:
        self._host: str | None = None

    async def _validate(self, host: str, token: str) -> tuple[dict[str, Any] | None, str | None]:
        api = SaltoApi(async_get_clientsession(self.hass), host, token)
        try:
            await api.check_auth()
            status = await api.get_status()
        except SaltoAuthError:
            return None, "invalid_auth"
        except SaltoError:
            return None, "cannot_connect"
        if "status_code" not in status or "mac" not in status:
            return None, "not_salto"
        return status, None

    async def async_step_user(self, user_input: dict[str, Any] | None = None) -> ConfigFlowResult:
        errors: dict[str, str] = {}
        if user_input is not None:
            host = user_input[CONF_HOST].strip()
            status, err = await self._validate(host, user_input[CONF_TOKEN])
            if err:
                errors["base"] = err
            else:
                await self.async_set_unique_id(_norm_mac(status["mac"]))
                self._abort_if_unique_id_configured(updates={CONF_HOST: host})
                return self.async_create_entry(
                    title="Salto", data={CONF_HOST: host, CONF_TOKEN: user_input[CONF_TOKEN]}
                )
        schema = vol.Schema(
            {
                vol.Required(CONF_HOST, default=(user_input or {}).get(CONF_HOST, self._host or "")): str,
                vol.Required(CONF_TOKEN): str,
            }
        )
        return self.async_show_form(step_id="user", data_schema=schema, errors=errors)

    async def async_step_zeroconf(self, discovery_info: ZeroconfServiceInfo) -> ConfigFlowResult:
        host = discovery_info.host
        api = SaltoApi(async_get_clientsession(self.hass), host, "")
        try:
            status = await api.get_status()  # open when READONLY_STATUS_OPEN=1
        except SaltoError:
            status = {}
        if status and "status_code" not in status:
            return self.async_abort(reason="not_salto")
        mac = _norm_mac(status.get("mac"))
        if mac:
            await self.async_set_unique_id(mac)
            self._abort_if_unique_id_configured(updates={CONF_HOST: host})
        self._host = host
        self.context["title_placeholders"] = {"host": host}
        return await self.async_step_user()

    async def async_step_reauth(self, entry_data: Mapping[str, Any]) -> ConfigFlowResult:
        return await self.async_step_reauth_confirm()

    async def async_step_reauth_confirm(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        entry = self._get_reauth_entry()
        errors: dict[str, str] = {}
        if user_input is not None:
            _, err = await self._validate(entry.data[CONF_HOST], user_input[CONF_TOKEN])
            if err:
                errors["base"] = err
            else:
                return self.async_update_reload_and_abort(
                    entry, data_updates={CONF_TOKEN: user_input[CONF_TOKEN]}
                )
        return self.async_show_form(
            step_id="reauth_confirm",
            data_schema=vol.Schema({vol.Required(CONF_TOKEN): str}),
            errors=errors,
        )
