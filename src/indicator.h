#pragma once
//
// Indicator: the two on-board LEDs (no camera, no OLED).
//
//   Red LED (GPIO33, active LOW) = "is the firmware up":
//     blinking  - starting (from the first line of setup() until the first Wi-Fi IP)
//     lit       - everything started
//   Flash LED (GPIO4) = errors only, in short patterns (pulses are PWM-dimmed to
//   spare the supply; full brightness can brown out a weak USB source):
//     3 short pulses - SD card missing (not mounted)
//     1 long pulse   - SD card faulty (mounted, writes fail)
//     2 short pulses - Wi-Fi is down (configured, not connected for over 30 s)
//   Several errors play one after another; with no error the flash LED stays dark.
//
namespace Indicator {

// First thing in setup(): red starts blinking, the error task starts.
void begin();

// Startup is complete (first Wi-Fi IP, or nothing to wait for): red turns steady.
void ready();

// Wi-Fi state for the error pattern. configured=false means Wi-Fi is not used.
void wifiConfigured(bool configured);
void wifiUp(bool up);

}  // namespace Indicator
