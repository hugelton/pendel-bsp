# pendel-bsp

The hardware layer of [Pendel](https://github.com/hugelton/Pendel), firmware for the M-VAVE Pocket AMP.

A branch of [felucca-bsp](https://github.com/hugelton/felucca-bsp), with its history: the chip layer for JieLi's
AC79 family (clock, interrupts, flash, USB, timers, boot entry), plus the Pocket AMP's board. No sound code and no
user interface.

Unofficial. Not affiliated with or endorsed by M-VAVE or JieLi. Product and company names belong to their owners.

## What this branch adds

| Path | What |
| --- | --- |
| `boards/pocketamp/pa_audio.h` | The audio link: I2S on port A, full duplex (out on channel 0, in on channel 1), 24-bit samples; the board's other inputs (the knob's encoder, the volume knob, the battery) |
| `boards/pocketamp/pa_input.h` | The six buttons, read directly with debounce |
| `boards/pocketamp/pa_lcd_hw.h`, `pa_lcd_init.h` | The 240 x 240 panel on SPI1 and its backlight |
| `boards/pocketamp/pa_loader.c` | The update loader for the Pocket AMP: `loader/` with this board's flash layout, USB names and panel |
| `examples/pa-hwtest/` | A test firmware for a unit: screen, buttons, audio, USB serial ([README](examples/pa-hwtest/README.md)) |

Everything else follows felucca-bsp: its [README](https://github.com/hugelton/felucca-bsp) has the layout, the FM-1
board and the experimental parts.

## Used by

- [Pendel](https://github.com/hugelton/Pendel): firmware for the M-VAVE Pocket AMP, on this repository
- [Felucca](https://github.com/hugelton/Felucca): firmware for the M-VAVE FM-1, on felucca-bsp

## Use

Pendel takes this repository as its submodule `firmware/bsp`. Include by file name: `-Ichip -Iboards/pocketamp
-Iboards/fm1`. Building needs the JieLi pi32v2 toolchain (Linux).

## Licence

[MPL-2.0](LICENSE), per file: change a file from here and distribute it, and you publish your change to that file;
your own code on top may use any licence, GPL included. Credits and origins: [NOTICE](NOTICE),
[LICENSING.md](LICENSING.md).
