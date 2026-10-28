# CoCIS Smart System — One ESP32 Wokwi Demonstration

This project implements the entrance, lecture room 1, and lecture room 2 on **one ESP32**. Separate FreeRTOS tasks simulate the three terminals; a queue passes room updates to the entrance controller. It is **not** a multi-MCU distributed build.

## Build in VS Code

```bash
pio run -e standalone
```

Then use **Wokwi: Start Simulator**. `wokwi.toml` already points to the standalone build. The `sdcard/` folder supplies the FAT card contents in Wokwi for VS Code.

## Features

- Two RC522 RFID readers and two door servos for entry and exit
- Two independent PIR-triggered room lights (30-second inactivity timeout)
- DS1307-compatible RTC / software clock fallback, configurable same-day access hours
- Admin-protected settings including clock, schedule, tags, and emergency exit override
- Building roster persisted to ESP32 NVS on each entry/exit
- SD card access log and tag registry; SD mount failures never autoformat data
- Serial commands, LCD, status LEDs, buzzer and intruder alert
- FreeRTOS queue and mutexes protect shared resources

## Serial demonstration

1. Run `help`, `time`, `schedule`, `status`.
2. Run `login admin123` (demo password; change with `passwd NEW_PASSWORD`).
3. Run `setdate 28 10 26` and `settime 14 00 00` to set the demo clock.
4. Use `tags`, `tap in 01020304`, `inside`, `tap out 01020304`, `log`.
5. Trigger each PIR sensor to show the two lights independently.
6. Run `setclose 14:01`, advance time to 14:02, and test an access denial. Use `override 5` to demonstrate admin-authorized exit.
7. Restart the simulation and check `inside` for NVS roster persistence (only if simulator preserves flash state).

**Note:** `scan` and RFID polling are serialized with an RFID mutex. PIR sensors estimate occupancy from motion and do not reliably detect still people. NVS roster writes may wear flash over very large numbers of events; suitable for this pilot/demo.

## Online submission

See `ONLINE_WOKWI_SUBMISSION.md`. Wokwi Online supports manually uploading a merged ESP32 `.bin` and starting a simulation, but **does not document permanent retention of uploaded firmware in a public project**. You must verify that a saved project link starts independently in a private window. If not, ask your lecturer to accept the source repository plus firmware upload instructions or move to a Wokwi web-compilable implementation.
