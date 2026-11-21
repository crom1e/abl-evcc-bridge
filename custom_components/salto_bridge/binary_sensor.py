"""Binary sensors for Salto Bridge."""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass
from typing import Any

from homeassistant.components.binary_sensor import (
    BinarySensorDeviceClass,
    BinarySensorEntity,
    BinarySensorEntityDescription,
)
from homeassistant.const import EntityCategory
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from . import SaltoConfigEntry
from .entity import SaltoEntity


@dataclass(frozen=True, kw_only=True)
class SaltoBinaryDescription(BinarySensorEntityDescription):
    value_fn: Callable[[dict[str, Any]], bool | None]


BINARY_SENSORS: tuple[SaltoBinaryDescription, ...] = (
    SaltoBinaryDescription(
        key="charging",
        device_class=BinarySensorDeviceClass.BATTERY_CHARGING,
        value_fn=lambda d: d.get("_charging"),
    ),
    SaltoBinaryDescription(
        key="car_connected",
        device_class=BinarySensorDeviceClass.PLUG,
        value_fn=lambda d: d.get("_connected"),
    ),
    SaltoBinaryDescription(
        # Link between the ESP bridge and the charger (RS485). Bridge unreachable = all entities unavailable.
        key="charger_link",
        device_class=BinarySensorDeviceClass.CONNECTIVITY,
        entity_category=EntityCategory.DIAGNOSTIC,
        value_fn=lambda d: d.get("online"),
    ),
)


async def async_setup_entry(
    hass: HomeAssistant,
    entry: SaltoConfigEntry,
    async_add_entities: AddConfigEntryEntitiesCallback,
) -> None:
    coordinator = entry.runtime_data
    async_add_entities(SaltoBinarySensor(coordinator, desc) for desc in BINARY_SENSORS)


class SaltoBinarySensor(SaltoEntity, BinarySensorEntity):
    entity_description: SaltoBinaryDescription

    def __init__(self, coordinator, description: SaltoBinaryDescription) -> None:
        super().__init__(coordinator, description.key)
        self.entity_description = description

    @property
    def is_on(self) -> bool | None:
        return self.entity_description.value_fn(self.coordinator.data)
