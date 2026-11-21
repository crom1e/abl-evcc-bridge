"""Sensors for Salto Bridge."""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass
from typing import Any

from homeassistant.components.sensor import (
    SensorDeviceClass,
    SensorEntity,
    SensorEntityDescription,
    SensorStateClass,
)
from homeassistant.const import (
    SIGNAL_STRENGTH_DECIBELS_MILLIWATT,
    EntityCategory,
    UnitOfElectricCurrent,
    UnitOfPower,
)
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from . import SaltoConfigEntry
from .const import STATUS_BY_CODE, STATUS_OPTIONS, VOLTAGE
from .entity import SaltoEntity


def _status(d: dict[str, Any]) -> str | None:
    code = d.get("status_code")
    if code is None:
        return None
    return STATUS_BY_CODE.get(code, "unknown")


def _applied_power(d: dict[str, Any]) -> float | None:
    a = d.get("applied_a")
    return round(a * VOLTAGE) if a is not None else None


@dataclass(frozen=True, kw_only=True)
class SaltoSensorDescription(SensorEntityDescription):
    value_fn: Callable[[dict[str, Any]], Any]


SENSORS: tuple[SaltoSensorDescription, ...] = (
    SaltoSensorDescription(
        key="status",
        device_class=SensorDeviceClass.ENUM,
        options=STATUS_OPTIONS,
        value_fn=_status,
    ),
    SaltoSensorDescription(
        key="charging_current",
        device_class=SensorDeviceClass.CURRENT,
        native_unit_of_measurement=UnitOfElectricCurrent.AMPERE,
        state_class=SensorStateClass.MEASUREMENT,
        suggested_display_precision=1,
        value_fn=lambda d: d.get("applied_a"),
    ),
    SaltoSensorDescription(
        key="charging_power",
        device_class=SensorDeviceClass.POWER,
        native_unit_of_measurement=UnitOfPower.WATT,
        state_class=SensorStateClass.MEASUREMENT,
        suggested_display_precision=0,
        value_fn=_applied_power,
    ),
    SaltoSensorDescription(
        key="default_current",
        device_class=SensorDeviceClass.CURRENT,
        native_unit_of_measurement=UnitOfElectricCurrent.AMPERE,
        entity_category=EntityCategory.DIAGNOSTIC,
        suggested_display_precision=1,
        value_fn=lambda d: d.get("default_a"),
    ),
    SaltoSensorDescription(
        key="status_code",
        entity_category=EntityCategory.DIAGNOSTIC,
        entity_registry_enabled_default=False,
        value_fn=lambda d: d.get("status_code"),
    ),
    SaltoSensorDescription(
        key="charger_firmware",
        entity_category=EntityCategory.DIAGNOSTIC,
        value_fn=lambda d: d.get("_charger_fw"),
    ),
    SaltoSensorDescription(
        key="wifi_signal",
        device_class=SensorDeviceClass.SIGNAL_STRENGTH,
        native_unit_of_measurement=SIGNAL_STRENGTH_DECIBELS_MILLIWATT,
        state_class=SensorStateClass.MEASUREMENT,
        entity_category=EntityCategory.DIAGNOSTIC,
        entity_registry_enabled_default=False,
        value_fn=lambda d: d.get("rssi"),
    ),
)


async def async_setup_entry(
    hass: HomeAssistant,
    entry: SaltoConfigEntry,
    async_add_entities: AddConfigEntryEntitiesCallback,
) -> None:
    coordinator = entry.runtime_data
    async_add_entities(SaltoSensor(coordinator, desc) for desc in SENSORS)


class SaltoSensor(SaltoEntity, SensorEntity):
    entity_description: SaltoSensorDescription

    def __init__(self, coordinator, description: SaltoSensorDescription) -> None:
        super().__init__(coordinator, description.key)
        self.entity_description = description

    @property
    def native_value(self) -> Any:
        return self.entity_description.value_fn(self.coordinator.data)
