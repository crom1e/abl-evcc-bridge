"""Charging current limit as a number entity (6..MAX_A, set by the bridge with "!0 12")."""

from __future__ import annotations

from homeassistant.components.number import NumberDeviceClass, NumberEntity, NumberMode
from homeassistant.const import UnitOfElectricCurrent
from homeassistant.core import HomeAssistant
from homeassistant.exceptions import HomeAssistantError
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from . import SaltoConfigEntry
from .api import SaltoError
from .entity import SaltoEntity

DEFAULT_MIN_A = 6
DEFAULT_MAX_A = 16


async def async_setup_entry(
    hass: HomeAssistant,
    entry: SaltoConfigEntry,
    async_add_entities: AddConfigEntryEntitiesCallback,
) -> None:
    async_add_entities([SaltoCurrentLimit(entry.runtime_data)])


class SaltoCurrentLimit(SaltoEntity, NumberEntity):
    """Current limit offered to the car. The range is also enforced by the bridge (6..MAX_A)."""

    _attr_device_class = NumberDeviceClass.CURRENT
    _attr_native_unit_of_measurement = UnitOfElectricCurrent.AMPERE
    _attr_native_step = 1
    _attr_mode = NumberMode.SLIDER

    def __init__(self, coordinator) -> None:
        super().__init__(coordinator, "current_limit")

    @property
    def native_min_value(self) -> float:
        return float(self.coordinator.data.get("min_a") or DEFAULT_MIN_A)

    @property
    def native_max_value(self) -> float:
        return float(self.coordinator.data.get("max_a") or DEFAULT_MAX_A)

    @property
    def native_value(self) -> float | None:
        amps = self.coordinator.data.get("applied_a")
        if amps is None:
            return None
        # Outside the slider range (0 = no current, or a value the coordinator did not catch):
        # report unknown rather than a number an automation could act on.
        if not self.native_min_value <= amps <= self.native_max_value:
            return None
        return amps

    async def async_set_native_value(self, value: float) -> None:
        amps = int(round(value))
        try:
            data = await self.coordinator.api.set_current(amps)
        except SaltoError as err:
            raise HomeAssistantError(f"Could not set charging current to {amps} A: {err}") from err
        # The response contains the new status (with read-back current) - use it directly
        self.coordinator.async_set_updated_data(self.coordinator.derive(data))
