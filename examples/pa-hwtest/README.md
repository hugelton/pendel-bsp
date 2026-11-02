# pa-hwtest

The first firmware for a Pocket AMP: it checks the board files in `boards/pocketamp/` one part at a time. It is a
test, not an amp. The board files were checked with it on a unit.

| Part | What you see |
| --- | --- |
| Screen | Six colour bars at the top (red, green, blue, white, yellow, cyan), a white mark in the top-left corner: the panel, its colour order and its orientation. |
| Buttons | Six cells light while a button is held. |
| Audio | A colour strip shows the mode; a bar shows the input's level (red bar: the audio link is not running). |
| USB | CDC serial, 115200 8N1 (the rate is ignored): a status line every 500 ms. |
| Corner | Bottom right: red = USB not configured, blinking green = configured. Yellow mark at the bottom: the last run crashed. |

Serial commands: `h` help, `p` print, `c` last crash, `t` tone (440 Hz, -12 dBFS, both sides), `l` / `R` left / right
only, `i` input to output, `m` quiet, `9` toggle PA9, `u` UBOOT, `r` reboot. The SysEx soft key (`F0 22 24 35 7D F7`)
enters UBOOT too.

Safe by design: nothing in it writes the flash, the supply levels stay as the boot loader set them, and two boots in a
row that do not reach the main loop's healthy point (2 s) drop into UBOOT.

## Build

    JIELI_TOOLCHAIN=~/.fm1/jieli/toolchain examples/pa-hwtest/build.sh      # -> build/pa-hwtest.bin (~12 KB)

## Install

Back up the unit's whole flash first (UBOOT). Then write the test through UBOOT, and write the backup back afterwards.

## Licence

MPL-2.0, like the rest of the repository.
