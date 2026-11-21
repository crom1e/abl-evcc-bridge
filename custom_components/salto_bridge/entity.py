"""Common base class for Salto Bridge entities."""

from __future__ import annotations

from homeassistant.helpers.device_registry import CONNECTION_NETWORK_MAC, DeviceInfo
from homeassistant.helpers.update_coordinator import CoordinatorEntity

from .const import DOMAIN, MANUFACTURER, MODEL
from .coordinator import SaltoCoordinator


class SaltoEntity(CoordinatorEntity[SaltoCoordinator]):
    """Entity attached to the Salto device."""

    _attr_has_entity_name = True

    def __init__(self, coordinator: SaltoCoordinator, key: str) -> None:
        super().__init__(coordinator)
        uid = coordinator.config_entry.unique_id or coordinator.config_entry.entry_id
        self._attr_unique_id = f"{uid}_{key}"
        self._attr_translation_key = key
        data = coordinator.data or {}
        mac = data.get("mac")
        self._attr_device_info = DeviceInfo(
            identifiers={(DOMAIN, uid)},
            connections={(CONNECTION_NETWORK_MAC, mac.lower())} if mac else set(),
            name="Salto",
            manufacturer=MANUFACTURER,
            model=MODEL,
            sw_version=data.get("fw_version"),
            configuration_url=f"http://{coordinator.api.host}/",
        )
