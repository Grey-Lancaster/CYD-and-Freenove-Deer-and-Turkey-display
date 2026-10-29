## Easiest: the web installer

### [➡ Open the Web Installer](https://grey-lancaster.github.io/CYD-and-Freenove-Deer-and-Turkey-display/)

Use **Chrome or Edge on a computer**, plug the board in by USB, pick your board in the list and click Connect, then Flash. (Freenove boards: hold BOOT, tap RESET, release BOOT first; press RESET after flashing.) No software to install. The files below are for updating a board that is already running, or for flashing by hand.

## Pick your board

| Build | Board | Display |
| --- | --- | --- |
| `freenove4` | **Freenove 4.0"** (FNK0104S) | ESP32-S3, ST7796 480×320, capacitive touch |
| `freenove28` | **Freenove 2.8"** (FNK0104B/AB) | ESP32-S3, ILI9341 320×240, capacitive touch, speaker |
| `cyd28` | **CYD 2.8"** (ESP32-2432S028) | ILI9341 display (the standard one-USB board) |
| `cyd28usb2` | **CYD 2.8" with an ST7789 display** | the 2-USB variant (starts with red/blue swapped, as those panels need) |

Colours wrong on a CYD? You most likely picked the wrong CYD build; try the other one. The web page also has **Swap red/blue** and **Invert colours** switches. Nothing is damaged.

![Deer Hunt](https://raw.githubusercontent.com/Grey-Lancaster/CYD-and-Freenove-Deer-and-Turkey-display/main/docs/img/screenshot.png)

## Which file do I want?
| File | Use it to | WiFi and settings |
| --- | --- | --- |
| `Deer-Hunt-<build>-factory.bin` | install on a new board, or recover one | **wiped** (you set up WiFi again on the screen) |
| `Deer-Hunt-<build>-update.bin` | update a board that is already running Deer Hunt | **kept** |

**Factory file:** put the board in bootloader mode (Freenove: hold BOOT, tap RESET, release BOOT) and flash it at offset 0, for example
`esptool.py --chip esp32s3 write_flash 0x0 Deer-Hunt-freenove28-factory.bin` (use `--chip esp32` for the two CYD builds), then press RESET.

**Update file:** open the board's web page (`http://deerhunt.local`; Freenove 2.8": `http://deerhunt28.local`; CYD: `http://cyddeerhunt.local`), press **Firmware update**, choose the file for your board and let it upload; it restarts by itself. From a computer: `espota.py -i <ip> -p 3232 -f Deer-Hunt-<build>-update.bin -r`. Only use a file whose name contains your own build name.

## Credits
The woods ambience is derived from "Rural Pennsylvania wooded ambience background with wild turkeys, Carolina wren, American crow" by **Jon5515** ([freesound.org/s/871327](https://freesound.org/s/871327/)), licensed [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/). Changes: the first 25 seconds, mixed to mono, noise-reduced, with the cricket band filtered out, looped with a crossfade.
