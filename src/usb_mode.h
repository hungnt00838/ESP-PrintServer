// What the board's USB port (the "USB 5V/IN" one) is used for:
//   print mode (default): USB host for the printer, after the reflash window
//   flash mode: stays the chip's USB-Serial/JTAG, for flashing over USB
// Holding a volume button (GPIO39 / GPIO40) for 2 s switches between them,
// with a quick restart (the USB PHY changes hands at boot only). Flash mode
// lasts until switched back, a power cycle, or a flash (any reset other than
// that restart).
#pragma once

#include <stdbool.h>

// Reads the mode for this boot and starts watching the buttons.
void usb_mode_init(void);
bool usb_mode_flash(void);
// Switches mode (restarts the board); nothing if already in it.
void usb_mode_set(bool flash);
