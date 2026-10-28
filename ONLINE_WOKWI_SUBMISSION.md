# Publishing one Wokwi Online link

## Build a full firmware image

Build in VS Code: `pio run -e standalone`.

Find the three files under `.pio/build/standalone/` (paths can differ by PlatformIO version):
- `bootloader.bin` (or `bootloader/bootloader.bin`)
- `partitions.bin` (or `partition_table/partition-table.bin`)
- `firmware.bin`

Check `.pio/build/standalone/flash_args` or `flasher_args.json` to verify offsets. For the usual ESP32 offsets (0x1000, 0x8000, 0x10000), run:

```bash
esptool --chip esp32 merge-bin -o merged.bin --flash-mode dio --flash-size 4MB \
  0x1000 PATH_TO_BOOTLOADER.bin \
  0x8000 PATH_TO_PARTITIONS.bin \
  0x10000 .pio/build/standalone/firmware.bin
```

Use the actual flash size and offsets from your build, not blindly the examples.

## Online steps

1. Sign into https://wokwi.com/projects/new/esp32 and create a project.
2. Replace its `diagram.json` with this project's `diagram.json`.
3. Use the ESP32 custom application firmware upload action: press F1 in the online editor and choose **Upload Firmware and Start Simulation…**; select `merged.bin`.
4. Test RFID, PIRs, and serial commands. Save the Wokwi project and copy the URL.
5. **Critical:** open the saved URL in a private/incognito window. If the uploaded firmware is missing, that link is NOT a self-running submission. Do not claim it is.
6. Also test SD card availability and whether its contents persist online. Wokwi VS Code `[[sdcard]]` writeback settings do not automatically transfer to a browser project.

## If the link cannot retain custom firmware

The Wokwi Online custom firmware upload flow is documented as an upload-and-run feature, not guaranteed permanent binary hosting. A dependable *one-link, click-and-run* submission may require rewriting the program as a Wokwi Online web-compilable Arduino sketch, which is a separate implementation and needs full retesting. Alternatively seek permission to submit the Wokwi circuit link alongside a GitHub repository and `merged.bin` with upload steps.

Official references:
- https://docs.wokwi.com/guides/esp32
- https://docs.wokwi.com/vscode/project-config
- https://docs.wokwi.com/faq
