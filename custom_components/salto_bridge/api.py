"""Client for the REST API exposed by the salto_bridge firmware."""

from __future__ import annotations

import asyncio
from typing import Any

import aiohttp

REQUEST_TIMEOUT = 10


class SaltoError(Exception):
    """The bridge could not be reached or returned an invalid response."""


class SaltoAuthError(SaltoError):
    """Wrong or missing token."""


class SaltoCommandError(SaltoError):
    """The bridge reached the charger, but the command was rejected."""


class SaltoApi:
    """Thin async client for the ESP bridge."""

    def __init__(self, session: aiohttp.ClientSession, host: str, token: str) -> None:
        self._session = session
        self._host = host
        self._base = f"http://{host}"
        self._token = token

    @property
    def host(self) -> str:
        return self._host

    async def _request(
        self, method: str, path: str, json: dict[str, Any] | None = None
    ) -> tuple[int, dict[str, Any]]:
        headers = {"Authorization": f"Bearer {self._token}"} if self._token else None
        try:
            async with asyncio.timeout(REQUEST_TIMEOUT):
                async with self._session.request(
                    method, self._base + path, json=json, headers=headers
                ) as resp:
                    if resp.status == 401:
                        raise SaltoAuthError("invalid token")
                    try:
                        data = await resp.json(content_type=None)
                    except (aiohttp.ContentTypeError, ValueError) as err:
                        raise SaltoError(f"invalid response from {self._base}{path}") from err
                    if not isinstance(data, dict):
                        raise SaltoError(f"unexpected response from {self._base}{path}")
                    return resp.status, data
        except (aiohttp.ClientError, TimeoutError) as err:
            raise SaltoError(f"cannot reach {self._base}: {err}") from err

    async def get_status(self) -> dict[str, Any]:
        """GET /api/status."""
        status, data = await self._request("GET", "/api/status")
        if status != 200:
            raise SaltoError(f"/api/status returned HTTP {status}")
        return data

    async def check_auth(self) -> None:
        """GET /api/auth - validates the token without sending anything to the charger."""
        status, _ = await self._request("GET", "/api/auth")
        if status != 200:
            raise SaltoError(f"/api/auth returned HTTP {status} (firmware too old?)")

    async def set_current(self, amps: int) -> dict[str, Any]:
        """POST /api/current. Returns the new status on success."""
        status, data = await self._request("POST", "/api/current", json={"amps": amps})
        if status == 200 and data.get("command_ok"):
            return data
        raise SaltoCommandError(
            data.get("command_error") or data.get("error") or f"HTTP {status}"
        )
