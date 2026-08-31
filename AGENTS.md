# ESP32-CAM Car Project Guidance

## Project purpose

- This is a learning and portfolio project for a mechanical-engineering undergraduate approaching the third year.
- The goals are to learn embedded development, electronics debugging, Git, and GitHub while building a four-wheel ESP32-CAM car.
- Prefer work that produces clear engineering evidence: reproducible tests, wiring notes, measured results, meaningful commits, and reviewable pull requests.

## How to work with the user

- Communicate in concise, beginner-friendly Chinese unless the user requests another language.
- Before step-by-step troubleshooting, first present the overall objective, diagnostic plan, decision branches, risks, and completion criteria, then wait for the user's confirmation of the plan.
- For physical wiring and hardware tests, give exactly one action at a time.
- Explain briefly why the current action is needed and state its success criterion.
- Wait for the user's observation or photo before giving the next physical action.
- Do not infer terminal functions from module appearance. Read the actual silkscreen or request a clear photo first.
- Distinguish verified facts from estimates and hypotheses. Never report a hardware test as passed until the user confirms the observed result.

## Git and GitHub workflow

- Protect `main`: do not commit or push feature work directly to it.
- Create one branch per coherent task, not one branch per commit.
- Use descriptive engineering prefixes:
  - `feature/` for new functionality.
  - `test/` for planned verification work.
  - `fix/` for defect corrections.
  - `docs/` for documentation-only work.
  - `chore/` for configuration and maintenance.
- Do not use `codex/` or `agent/` branch prefixes in this repository.
- A task branch may contain several small, related commits. Use concise Conventional Commit-style subjects such as `feat:`, `test:`, `fix:`, `docs:`, or `chore:`.
- Stage only files that belong to the task. Preserve unrelated user changes.
- Before committing firmware changes, run `pio run` (or locate the active PlatformIO executable if it is not on `PATH`) and report the result.
- Hardware validation and compilation validation are different; record both when applicable.
- For every completed or explicitly requested snapshot, commit the intended files, push the branch to GitHub, and open or update a draft pull request.
- Before each commit or push, tell the user what will be saved so the Git workflow remains part of the learning process.
- Do not merge a pull request or delete a branch without telling the user. After a task is merged, normally delete its completed task branch.
- Never commit credentials, personal access tokens, private Wi-Fi credentials, or other secrets.

## Firmware and milestone preservation

- Keep verified milestones recoverable through Git. Do not overwrite a working milestone without first preserving it in a commit and remote branch.
- Prefer a separate minimal test mode or task branch when a hardware test temporarily needs different firmware.
- Use numbered project parts instead of calling milestones “days”; keep actual calendar dates inside each test record.
- Part 1 is verified: minimum system, PlatformIO build/upload, serial output, and heartbeat.
- Part 2 is verified on hardware: OV3660 camera, PSRAM, ESP32 access point, and live video at `http://192.168.4.1/`.
- The verified Part 2 camera implementation lives in commit `2a17b08` on `feature/day2-camera-stream` until its pull request is merged.
- Part 3 is verified: one unloaded motor completed both directions and stopped as programmed.
- Part 4 produced the four-motor automatic test firmware and preparation record, but did not complete four-motor hardware validation.
- Part 5 completed the four-motor electrical assembly and unpowered checks. It did not include a powered motor test.
- Part 6 independently verified the replacement ESP32-CAM, then verified all four unloaded motors follow the time-limited automatic sequence and stop reliably. A temporary Dupont connection caused one no-response attempt and recovered after reseating, but the exact loose connection remains unidentified; camera and PSRAM were not tested on the replacement module.
- Part 7 initial commit `75706f3` was compiled and uploaded by the user; the phone connected to the AP, the page and camera image appeared, and all four motors moved. The user observed laggy HTTP polling control, short travel under the 1.5 s limit, and one later video loss. A separate seller-firmware test with independent 5 V / 1 A power also worked, so the video loss was not treated as confirmed camera damage.
- Part 7 optimized revision `90647f6` uses the WebSocket/5 s/reconnect firmware introduced in commit `4240cf4`. The user completed a 10-minute hardware acceptance test on 2026-08-29 with 11 repeated operations. The hotspot, page, video, four motion commands, release stop, disconnected-control stop, action-limit stop, and video recovery all behaved as expected. The user observed no resets, abnormal heating, burning smell, runaway motion, or connection fault. Treat this as verified first-stage prototype behavior within that test coverage, not as a long-term reliability result.

## Confirmed hardware

- ESP32-CAM: ESP-32S module with AI Thinker-compatible camera pin layout.
- Download board: ESP32-CAM-MB; the serial device is commonly `/dev/cu.usbserial-10`, but verify it before upload.
- Camera sensor: OV3660.
- Motor drivers: two TC1508A dual-channel boards for the car and one spare; do not treat them as MX1508 boards.
- Motors: four yellow TT motors. Measured winding resistances are M1 8.0 ohm, M2 7.9 ohm, M3 7.1 ohm, and M4 7.3 ohm.
- Planned side pairing is M1 with M3 and M2 with M4.
- Motor supply: four series NiMH AA cells, nominally 4.8 V and measured around 5.1 V.
- ESP32-CAM supply: separate 5 V power from the ESP32-CAM-MB or a power bank.

## Hardware safety

- Before wiring or changing any connection, disconnect USB power and the motor battery supply.
- Keep the wheels removed or all motor shafts unloaded during initial multi-motor tests.
- Motor current must never pass through the ESP32-CAM board.
- Connect ESP32 GND and TC1508A GND when control signals are used, but never connect the positive terminals of the two independent supplies together.
- The breadboard and ordinary Dupont wires may carry only GPIO control signals and the logic ground reference, never motor current.
- GPIO12 and GPIO15 are boot-strapping pins. Keep motor power off while ESP32-CAM starts; connect motor power only after startup, and do not restart ESP32-CAM while the motor drivers remain powered.
- Do not measure stall current during the initial test, and never allow a motor to remain stalled.
- The estimated cold stall current for two parallel motors on one side is roughly 1.4-1.6 A; treat this only as an estimate until measured safely later.
- Bare TC1508A through-holes require properly soldered headers or terminals for reliable use. Do not rely on loose pins or thin Dupont leads for motor current.

## Maintaining this file

- Update this file when the user establishes a durable workflow preference, confirms a hardware fact, changes a safety boundary, or completes a verified milestone.
- Do not add transient conversation details, unverified guesses, secrets, or noisy command output.
- Keep changes concise and include them in the appropriate task branch and pull request so the history explains why the guidance changed.
