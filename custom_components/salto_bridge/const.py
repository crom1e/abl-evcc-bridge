"""Constants for Salto Bridge."""

from __future__ import annotations

from datetime import timedelta

DOMAIN = "salto_bridge"

CONF_TOKEN = "token"

SCAN_INTERVAL = timedelta(seconds=5)

# Nominal voltage used to convert the current limit into power (single-phase charger)
VOLTAGE = 230

# Status codes reported by "!0 02" (mapped by observation)
CODE_NO_CAR = "0000"
CODE_CONNECTED = "0004"
CODE_CHARGING = "0005"
CODE_TRANSITION = "0012"
CODE_DISABLED = "0064"

# Codes that mean a car is plugged in
CONNECTED_CODES = {CODE_CONNECTED, CODE_CHARGING, CODE_DISABLED}
# Codes that briefly appear during state transitions and must not immediately mean "no car"
TRANSIENT_CODES = {CODE_NO_CAR, CODE_TRANSITION}
# How long "no car" must persist before the car-connected sensor turns off
DISCONNECT_DEBOUNCE_S = 30

STATUS_OPTIONS = ["no_car", "connected", "charging", "disabled", "transition", "unknown"]
STATUS_BY_CODE = {
    CODE_NO_CAR: "no_car",
    CODE_CONNECTED: "connected",
    CODE_CHARGING: "charging",
    CODE_DISABLED: "disabled",
    CODE_TRANSITION: "transition",
}

MANUFACTURER = "ABL Sursum"
MODEL = "EVCC wallbox via ESP8266 RS485 bridge"
