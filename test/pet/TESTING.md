# Testing the `fixes` firmware on real hardware

This checks the fixes on the `fixes` branch against a real PET and PETdisk MAX.
It takes about 30 minutes. Please send back photos of the screen and the
details listed under "What to send back".

## What you need

- A PET with BASIC 4 (BASIC 2 works too; `DIRECTORY`/`CATALOG` need BASIC 4).
- A PETdisk MAX and a FAT32 microSD card.
- The right firmware file for your PETdisk:

  | PETdisk | Chip | File to copy to the card |
  |---|---|---|
  | v2 and v2 clones ("CBM Petdisk") | ESP32 | `FIRMWARE.PD2` |
  | bitfixer v3 | ESP32-S2 | `FIRMWARE.PD3` |

- A web server with PHP serving the new `www/petdisk.php` from this branch.
  Anything with PHP 7.4 or newer works: Apache or nginx on a PC or
  Raspberry Pi, a NAS's web server, or Docker (`startServer.sh` in the repo
  root serves the `www` folder on port 80).
  - Put `petdisk.php` in its own folder; that folder is the network drive.
  - Make a subfolder there called `PDTDIR` (for the subfolder test).
  - **Keep it on your home network.** Don't forward a port to it.
- `PDTEST.PRG` from this folder, copied to the SD card.

## Configure

`PETDISK.CFG` in the root of the SD card, for example:

```
8,SD0
9,192.168.1.20/petdisk/petdisk.php
ssid,YourNetwork
password,YourPassword
```

To test two network drives at once, add a second URL line with another
device number (say `10,`). It can point to the same script.

## Step 1: run the test on your current firmware

This gives a "before" picture to compare against.

1. Power on, then `LOAD"$",8` and `LIST`. **Photograph the first line**: it
   shows the firmware version.
2. `LOAD"PDTEST",8` and `RUN`.
3. Type the device numbers when asked (SD card, network, second network or 0).
4. Wait for `PASSED ... FAILED ... SKIPPED` (a few minutes). **Photograph it.**

## Step 2: install the new firmware

Optional but recommended if you have a USB-serial adapter (ESP32) or USB
cable (ESP32-S2): back up the current flash first.

```
esptool.py --chip esp32 -p COM5 read_flash 0 0x400000 petdisk-backup.bin
```

(Use `--chip esp32s2` for a v3. Replace `COM5` with your port.)

Then:

1. Copy `FIRMWARE.PD2` (or `.PD3`) to the root of the SD card.
2. Put the card in and power on. The PETdisk's loader writes the new
   firmware, restarts, and the firmware deletes the file from the card.
3. `LOAD"$",8` and `LIST`. The first line should show the new version.
   **Photograph it.**

## Step 3: run the test again

Same as step 1. **Photograph the result.**

Then try these by hand and note what happens:

- `SAVE"HELLO",8` then `DIRECTORY` and `CATALOG` (the files should be listed;
  this is the "blank directory after SAVE" report, issue #13).
- The same with device 9: `SAVE"HELLO",9`, `DIRECTORY ON U9`.
- `LOAD"$:PDTDIR",9` then `LIST`: shows the subfolder's files, with folders
  marked `DIR`. `LOAD"$:..",9` goes back up.
- `LOAD"$:NOSUCHDIR",9`: should stay in the current folder, not hang.

## What to send back

- The photos from steps 1, 2 and 3.
- Which PETdisk (v2 clone or bitfixer v3) and PET model.
- Your `PETDISK.CFG` **with the Wi-Fi password removed**.
- What the web server is (for example "Apache on a Raspberry Pi" or
  "Synology Web Station").

## If the PETdisk doesn't come back

- Take the SD card out, delete any `FIRMWARE.PD2`/`.PD3` on it, and power on.
- If it still doesn't respond, reflash the complete image over serial/USB:

  ```
  esptool.py --chip esp32 -p COM5 write_flash 0x0 image-esp32.bin
  ```

  (or your backup: `write_flash 0x0 petdisk-backup.bin`). On the v2 header
  the pins are `0V NC TX RX EN IO 3V 5V`; hold `IO` (GPIO0) to `0V` while
  connecting `EN` to `0V` and releasing it to enter the bootloader. The v3
  has USB.

## What the tests check

| Test | What it catches |
|---|---|
| SD SEQ 1500 | Writing and reading a 3-block file on the SD card |
| SD SAVE+DIR | `SAVE` to SD with a network drive configured, then the file is in the directory (issue #20, #13) |
| NET SEQ 1500 / 4000 | Multi-block network writes and reads, byte for byte |
| NET SAVE+DIR | `SAVE` to the network drive and listing it |
| NET MISSING FILE | Opening a file that doesn't exist must fail, not return garbage |
| NET<>SD COPY | Reading one device while writing the other, both ways |
| NET SUBFOLDER | `CD:` into a folder, write and list there, `CD←` back |
| TWO NET DRIVES | Two network drives read at the same time keep their own data |
