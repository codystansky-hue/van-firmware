// util.h -- small helpers shared by the hub's files.
#pragma once
#include <Arduino.h>
#include <math.h>

// a number for JSON: "null" when not available (NAN)
inline String jnum(float v, int decimals) { return isnan(v) ? String("null") : String(v, decimals); }

// text for HTML (names typed on the setup page, names advertised over Bluetooth)
inline String htmlEsc(const String& s) {
  String o;
  for (char c : s) {
    if (c == '<') o += "&lt;"; else if (c == '>') o += "&gt;"; else if (c == '&') o += "&amp;";
    else if (c == '"') o += "&quot;"; else if ((uint8_t)c >= 0x20) o += c;
  }
  return o;
}

inline String agoText(uint32_t ms) {
  uint32_t s = ms / 1000;
  return s < 90 ? String(s) + " s ago" : s < 5400 ? String(s / 60) + " min ago" : String(s / 3600) + " h ago";
}
