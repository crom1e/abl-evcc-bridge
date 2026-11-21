// types.h - types that must be known before Arduino's auto-generated function prototypes
#pragma once
#include <stdint.h>

// Last value read with the ABL ASCII protocol (raw field from the reply, e.g. "0267")
struct AblValue { bool have; char raw[12]; uint32_t ms; };

// What an RS485 job is for
enum Purpose : uint8_t {
  P_STATUS,  // !0 02  status
  P_CUR,     // !0 11  present current limit
  P_DEF,     // !0 26  default current
  P_FW,      // !0 01  firmware
  P_SET      // !0 12  set present current limit (the only write command)
};
