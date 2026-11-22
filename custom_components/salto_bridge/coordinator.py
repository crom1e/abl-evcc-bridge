"""DataUpdateCoordinator for Salto Bridge."""

from __future__ import annotations

import logging
import time
from typing import Any

from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant
from homeassistant.exceptions import ConfigEntryAuthFailed
from homeassistant.helpers.update_coordinator import DataUpdateCoordinator, UpdateFailed

from .api import SaltoApi, SaltoAuthError, SaltoError
from .const import (
    CODE_CHARGING,
    CONNECTED_CODES,
    DISCONNECT_DEBOUNCE_S,
    DOMAIN,
    SCAN_INTERVAL,
    TRANSIENT_CODES,
)

_LOGGER = logging.getLogger(__name__)


class SaltoCoordinator(DataUpdateCoordinator[dict[str, Any]]):
    """Polls /api/status every 5 seconds and adds derived fields."""

    config_entry: ConfigEntry

    def __init__(self, hass: HomeAssistant, entry: ConfigEntry, api: SaltoApi) -> None:
        super().__init__(
            hass,
            _LOGGER,
            name=DOMAIN,
            update_interval=SCAN_INTERVAL,
            config_entry=entry,
        )
        self.api = api
        self._last_connected: float | None = None
        self._charger_fw: str | None = None
        self._warned_current: set[Any] = set()

    async def _async_update_data(self) -> dict[str, Any]:
        try:
            data = await self.api.get_status()
        except SaltoAuthError as err:
            raise ConfigEntryAuthFailed(str(err)) from err
        except SaltoError as err:
            raise UpdateFailed(str(err)) from err
        return self.derive(data)

    def derive(self, data: dict[str, Any]) -> dict[str, Any]:
        """Add fields used by the entities. Also applied to the /api/current response."""
        data = dict(data)
        self._sanitize_current(data)
        code = data.get("status_code")
        now = time.monotonic()

        if code in CONNECTED_CODES:
            self._last_connected = now
            connected = True
        elif code in TRANSIENT_CODES and self._last_connected is not None:
            # 0000/0012 show up briefly during transitions - wait before reporting "no car"
            connected = now - self._last_connected < DISCONNECT_DEBOUNCE_S
        else:
            connected = False

        # The charger only reports its firmware while idle - keep the last known value
        if data.get("charger_fw"):
            self._charger_fw = data["charger_fw"]

        data["_connected"] = connected
        data["_charging"] = code == CODE_CHARGING
        data["_charger_fw"] = self._charger_fw
        return data

    def _sanitize_current(self, data: dict[str, Any]) -> None:
        """Never expose an impossible current limit (e.g. a corrupted read-back).

        The bridge already rejects such values; this is a second line of defence so an
        automation can never act on them. Valid: 0 (no current) or min_a..max_a.
        """
        amps = data.get("applied_a")
        if amps is None:
            return
        lo = data.get("min_a") or 6
        hi = data.get("max_a") or 16
        if amps == 0 or lo <= amps <= hi:
            return
        if amps not in self._warned_current:
            self._warned_current.add(amps)
            _LOGGER.warning(
                "Ignoring implausible current limit %s A from the bridge (raw %s, allowed 0 or %s-%s A)",
                amps,
                data.get("applied_raw"),
                lo,
                hi,
            )
        data["applied_a"] = None
        data["applied_raw"] = None
