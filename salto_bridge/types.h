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

// Kinds of entries in the persistent anomaly log
enum AnomalyKind : uint8_t {
  A_NONE = 0,
  A_INVALID,       // reply did not match the request or had a malformed value
  A_RANGE,         // current read-back outside 0 / 6..MAX_A - never published
  A_UNCONFIRMED,   // a changed value that a repeated read did not confirm (likely a bit error)
  A_STRAY,         // bytes on the bus that belonged to no request
  A_SUPPRESSED     // placeholder: N entries were not written (flash wear limit)
};

// One record in /anomaly.bin (fixed size)
struct AnomalyRec {
  uint32_t epoch;      // unix time, 0 = clock not set yet
  uint32_t uptimeS;
  uint32_t bootNo;
  uint16_t extra;      // A_SUPPRESSED: number of entries not written
  uint8_t  kind;       // AnomalyKind
  uint8_t  rxLen;
  char     cmd[12];    // request, e.g. "!0 11"
  char     stored[8];  // value the bridge held at the time
  uint8_t  rx[24];     // raw received bytes
};
