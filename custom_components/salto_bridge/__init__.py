"""Salto Bridge - ABL Sursum EVCC wallbox via an ESP8266 RS485 bridge."""

from __future__ import annotations

from homeassistant.config_entries import ConfigEntry
from homeassistant.const import CONF_HOST, Platform
from homeassistant.core import HomeAssistant
from homeassistant.helpers.aiohttp_client import async_get_clientsession

from .api import SaltoApi
from .const import CONF_TOKEN
from .coordinator import SaltoCoordinator

PLATFORMS: list[Platform] = [Platform.BINARY_SENSOR, Platform.NUMBER, Platform.SENSOR]

type SaltoConfigEntry = ConfigEntry[SaltoCoordinator]


async def async_setup_entry(hass: HomeAssistant, entry: SaltoConfigEntry) -> bool:
    api = SaltoApi(async_get_clientsession(hass), entry.data[CONF_HOST], entry.data[CONF_TOKEN])
    coordinator = SaltoCoordinator(hass, entry, api)
    await coordinator.async_config_entry_first_refresh()
    entry.runtime_data = coordinator
    await hass.config_entries.async_forward_entry_setups(entry, PLATFORMS)
    return True


async def async_unload_entry(hass: HomeAssistant, entry: SaltoConfigEntry) -> bool:
    return await hass.config_entries.async_unload_platforms(entry, PLATFORMS)
