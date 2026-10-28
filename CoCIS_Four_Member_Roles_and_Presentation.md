# CoCIS Smart Control and Monitoring System
## Four-Member Implementation Roles and Presentation Guide

**Coursework platform:** ESP32, ESP-IDF, FreeRTOS, Wokwi  
**Submission deadline:** 2 November 2026  
**Group size:** Four members  
**Presentation approach:** Each member explains and demonstrates their assigned implementation, not just theory.

> **Note:** File responsibilities below follow the group's modular single-ESP32 design. Confirm exact function names against the final working source before presenting. Every member should understand the complete project and attend the presentation.

## 1. Shared project objective

Build and demonstrate one ESP32 running three logical subsystems: (1) RFID-based building entrance and exit with occupancy tracking; (2) automatic lighting for Lecture Room 1; and (3) automatic lighting for Lecture Room 2. ESP-IDF and FreeRTOS coordinate these functions, with RTC scheduling, serial administration, persistent configuration and access logging.

## 2. Member assignments at a glance

| Member | Implementation ownership | Main files | Demonstration |
|---|---|---|---|
| **Member 1** | RFID, door servos, access decisions and occupancy | `access_control.c`, `rc522.c` (and relevant headers) | Authorized entry, denied entry, exit and occupancy |
| **Member 2** | RTC, access schedules, administration and storage | `system_manager.c`, `ds3231.c` (and relevant headers) | Admin commands, schedule, logs and stored settings |
| **Member 3** | PIR-based room automation and energy-saving behavior | `room_main.c`, room definitions in `common.h` | Room 1 and Room 2 motion, light-on and timeout |
| **Member 4** | ESP-IDF startup, FreeRTOS coordination and integration | `main.c`, `entrance_main.c`, `entrance_internal.h`, `CMakeLists.txt` | Boot, task/queue coordination, LCD/status and alarms |

## 3. Member 1 — RFID access control and occupancy

### What to study

- How the MFRC522 readers communicate with ESP32 over SPI, including shared SPI lines and separate chip-select pins.
- How a scanned UID is read and compared with registered tags.
- How entry and exit authorization decisions are made, including time restrictions and duplicate-entry/invalid-exit checks.
- How entry/exit servo motors are actuated and how access feedback is provided.
- How the occupancy roster is updated and protected when tasks access it concurrently.

### Code walkthrough

Open `access_control.c` and trace the path from **RFID scan → UID validation → access decision → servo action → occupancy update → log request**. Show the RFID driver initialization or read functions in `rc522.c` only as needed to explain SPI communication. Explain the purpose of the RFID mutex and how errors are handled.

### Wokwi demonstration checklist

- [ ] Present the two RFID readers and entry/exit servo connections.
- [ ] Demonstrate a registered tag entering successfully.
- [ ] Show the occupancy count/list changing.
- [ ] Demonstrate an unregistered tag being denied.
- [ ] Demonstrate exit and verify occupancy decreases.
- [ ] Explain what happens if the same person attempts to enter twice.

### Questions to prepare

1. Why are two RFID readers needed?
2. How does SPI chip select distinguish the readers?
3. What is a UID, and is a UID alone sufficient for high-security authentication?
4. How does the system avoid duplicate occupancy records?
5. Which shared data requires synchronization?

## 4. Member 2 — System management, RTC and persistence

### What to study

- RTC initialization and obtaining date/time.
- How configured access hours are stored and checked.
- Administrator authentication and the serial command interface.
- SD-card file mounting, access log format and tag registration storage.
- NVS persistence for configuration and occupancy, and what survives a restart.
- Failure handling when storage or RTC access fails.

### Code walkthrough

Open `system_manager.c` and explain **initialization → loading settings → receiving a serial command → validating administrator permissions → updating configuration or reporting status**. Trace one access-log write and one saved-setting update. Use `ds3231.c` to explain I²C time reads, without reading every low-level driver line.

### Wokwi demonstration checklist

- [ ] Show current date/time and explain the RTC.
- [ ] Show the serial menu and an authenticated administrator command.
- [ ] Display registered tags, occupancy or system status using available commands.
- [ ] Explain or demonstrate changing the access schedule.
- [ ] Show the SD-card log or describe its generated records.
- [ ] Explain NVS versus SD-card storage.

### Questions to prepare

1. Why use an RTC instead of only `xTaskGetTickCount()`?
2. What is the difference between NVS and SD-card storage?
3. Which serial commands require authentication?
4. What happens if the clock is incorrect or storage fails?
5. What information should an access log contain?

## 5. Member 3 — Lecture-room automation and energy saving

### What to study

- PIR sensor GPIO initialization and digital motion readings.
- How Room 1 and Room 2 use separate task contexts.
- How motion changes room state and switches the corresponding light on.
- How the 30-second inactivity timeout is calculated using FreeRTOS ticks.
- How room-status updates and periodic heartbeats reach the central queue.
- What the motion-event count represents and how estimated energy savings are calculated, if enabled in the final code.

### Code walkthrough

Open `room_main.c` and explain the room context structure, light-control function, monitoring loop, and `vTaskDelay(pdMS_TO_TICKS(100))`. Trace **PIR HIGH → update last-motion tick → mark occupied → light ON → inactivity timeout → light OFF**. Explain why a PIR sensor indicates motion rather than reliably counting people.

### Wokwi demonstration checklist

- [ ] Identify PIR and light pins for each room.
- [ ] Trigger Room 1 motion and show its light switching on.
- [ ] Trigger Room 2 independently and show its light switching on.
- [ ] Stop motion and wait for the inactivity timeout.
- [ ] Show that lights switch off after approximately 30 seconds without motion.
- [ ] Show room status or motion-event reports in the serial output.

### Questions to prepare

1. Why use one task per room?
2. Why convert milliseconds to FreeRTOS ticks?
3. How does the system decide a room is empty?
4. Can PIR sensors count occupants accurately?
5. What happens if motion is detected again before the timeout?

## 6. Member 4 — ESP-IDF startup, FreeRTOS and integration

### What to study

- `app_main()` and NVS initialization in `main.c`.
- The coordinator's hardware initialization order in `entrance_main.c`.
- Task creation, stack sizes, priorities and scheduling.
- FreeRTOS queue creation and room-message receiving.
- Mutexes for shared state, RFID, SD card, LCD and door operations.
- LCD updates, status/alarm coordination and overall error handling.
- Source registration in `CMakeLists.txt` and ESP-IDF build settings.

### Code walkthrough

Trace **ESP32 boot → `app_main()` → NVS initialization → coordinator startup → drivers and mutexes → queue → room tasks and other tasks**. Explain `xTaskCreate`, `xQueueCreate`, `xQueueReceive`, `xSemaphoreTake` and `xSemaphoreGive` using actual examples from the project. Clarify that one physical ESP32 runs three logical control subsystems.

### Wokwi demonstration checklist

- [ ] Start the firmware and show the boot sequence.
- [ ] Identify the single ESP32 and connected peripherals.
- [ ] Show LCD/status messages and any alarm indication.
- [ ] Show serial output from concurrent room and RFID activities.
- [ ] Explain where queue messages are received and displayed.
- [ ] Describe one race condition that a mutex prevents.

### Questions to prepare

1. Why ESP-IDF rather than Arduino?
2. What is the difference between a task and a normal function?
3. Why use a queue for room-status messages?
4. What is a race condition and how does a mutex prevent it?
5. Why initialize shared resources before creating tasks?

## 7. Recommended presentation order and timing

For a **20-minute presentation**, use approximately five minutes each. Adjust to the lecturer's actual time limit.

| Time | Presenter | Focus |
|---|---|---|
| 0:00–5:00 | **Member 4** | Project introduction, architecture, ESP-IDF startup and FreeRTOS integration |
| 5:00–10:00 | **Member 1** | RFID access control, door operation and occupancy demonstration |
| 10:00–15:00 | **Member 2** | RTC, serial administration, SD/NVS and schedules demonstration |
| 15:00–20:00 | **Member 3** | PIR lighting, room tasks, energy-saving behavior and conclusion |

**Suggested structure for each five-minute section:** 45 seconds to introduce the feature; 2 minutes to explain key functions and logic; 1 minute 30 seconds for Wokwi demonstration; 45 seconds for design decisions, limitations and transition. A 30-second PIR timeout may require preparing the demo so waiting does not consume the entire slot.

## 8. Shared responsibilities before the presentation

All four members should:

- [ ] Read the final working source files, not only their own assigned module.
- [ ] Verify that the Wokwi circuit and ESP-IDF firmware run correctly.
- [ ] Agree on test RFID tags, admin credentials and demo commands without exposing real secrets.
- [ ] Rehearse transitions between presenters and handoff of the simulator/serial monitor.
- [ ] Prepare a backup demonstration plan if shared Wokwi Online firmware does not persist.
- [ ] Be ready to explain the use of **one ESP32**, **ESP-IDF**, **FreeRTOS tasks**, **queues** and **mutexes**.
- [ ] Confirm that all group members attend the lecturer's presentation.

## 9. Final group rehearsal plan

1. **Integration check:** Member 4 boots the system and confirms tasks, peripherals and status messages.
2. **Security test:** Member 1 demonstrates successful entry, denied entry, exit and occupancy changes.
3. **Management test:** Member 2 demonstrates serial commands, access scheduling and stored records.
4. **Room test:** Member 3 demonstrates independent PIR lighting and the inactivity timeout.
5. **Failure scenarios:** Together discuss an unknown tag, an incorrect RTC time, missing SD card and simultaneous task activity.
6. **Question round:** Each member answers at least two questions from another member's section.

## 10. Submission checklist

- [ ] Source code uses **ESP-IDF** and builds successfully.
- [ ] Wokwi simulation demonstrates the single-ESP32 system.
- [ ] All expected features are tested in the final version.
- [ ] Repository/source archive and firmware are backed up.
- [ ] Any Wokwi sharing limitation is resolved or explained to the lecturer.
- [ ] All four members are prepared to present before **2 November 2026**.

---

**Recommended rule:** Each member owns a code section for presentation, but the system is a shared group project. Everyone should be able to explain how their module interacts with the others.
