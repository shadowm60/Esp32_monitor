# Esp32_monitor: context for Claude Code sessions

Read `README.md` first for the hardware, the pages and the EEZ workflow.

## Project

- **Device:** Waveshare ESP32-S3-Touch-LCD-7, V1.2 (800x480, GT911, CH422G),
  ESP32-S3-N16R8.
- **Software:** ESP-IDF v6.1, LVGL 8.4, EEZ Studio 0.29 with EEZ Flow.
- **Sibling projects:** `~/git/FP_Dash` (CAN dash on the same 7" board) and
  `~/git/OBD_Dash` (5" board). `board`, `can_if`, the EEZ build tooling and the
  bridge pattern come from FP_Dash. Check how it solved something before
  reinventing it.

## Build / UI

```sh
. ~/.espressif/tools/activate_idf_v6.1.sh
idf.py build
tools/eez-build.sh      # regenerate components/hmi/src/ui (needs a desktop session)
```

- Never hand-edit `components/hmi/src/ui/*`. The `.eez-project` is the source
  of truth. Change it in EEZ Studio or with a small Python edit of the JSON:
  - 2-space indent, `ensure_ascii=False`
  - fresh UUID `objID`s for new widgets
  - then run `tools/eez-build.sh`
- `tools/eez_scaffold.py --force` regenerates the whole project and destroys
  the owner's EEZ edits. Don't run it unless asked.
- UI variables, actions and screens are reached **by name** (`bridge/uivar.c`,
  `bridge/nav.c`). The only `objects.*` uses are font overrides in `hmi.c`
  (`can_id_table`, `gps_sat_table`, `gps_raw_table`, `profile_climb_table`)
  and the panels the bridge draws into: `route_map` (`view_route.c`),
  `profile_chart` (`view_profile.c`) and `ride_turn_arrow` (`view_ride.c`).
- New pages: define them in `eez_scaffold.py` (PAGES, builder, VARIABLES,
  ACTIONS, PAGE_EXTRAS), then `tools/eez_add_page.py <page>` adds them to
  the existing project in place.
- `sdkconfig` is local. Defaults go in `sdkconfig.defaults`.

## Rules

- **Services never call LVGL.** Only `hmi/bridge` touches EEZ/LVGL, from
  lv_timers, events and actions in the LVGL task. Services expose a
  thread-safe status struct plus non-blocking commands.
- **Init order** (`main.c`):
  1. settings (NVS)
  2. CH422G
  3. SD
  4. gps_svc, io_svc (its GPS mode starts gps_svc), gpx_svc, canmon
  5. LCD/UI (before Wi-Fi/BLE: it needs large internal-RAM blocks)
  6. Wi-Fi, BLE
  7. CAN autostart
- **CH422G:** all pin writes go through `ch422g_write_pin` (it holds a mutex).
- **CAN ↔ USB:** `canmon` switches EXIO5 to CAN on start and back to USB on stop.
- **Internal RAM is the tight resource:**
  - 136 KB static headroom at the first build
  - The RGB bounce buffers, the mode-4 render buffer, the BT controller and
    Wi-Fi all need internal RAM.
  - Put big buffers in PSRAM (`heap_caps_*`, `xQueueCreateWithCaps`), big
    statics with `EXT_RAM_BSS_ATTR` (`CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY`),
    and stacks of tasks that never write flash with
    `xTaskCreatePinnedToCoreWithCaps(..., MALLOC_CAP_SPIRAM)` (gps_svc, io_svc).
  - The BT controller allocates last and asserts (`BLE_INIT: Malloc failed`,
    emi.c) when internal RAM runs out: the GPS page alone (+9 KB) tipped it over.
  - `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=0`: plain `malloc` (LVGL objects,
    EEZ values) goes to PSRAM. With the 16 KB default, the UI used up internal
    RAM and FreeRTOS failed to create a mutex (first board run).
  - The display initialises before Wi-Fi/BLE so it gets its contiguous
    blocks. Boot logs `after <step> internal free ...` per step.
- **Owner tests on hardware.** Say clearly what was only built, not run.

## Status (2026-11-13)

First version, builds with no warnings in project code. **On the board:**
boots with Wi-Fi + BLE + display, Wi-Fi connects, SNTP sets the clock, and BLE
scan finds devices. Still to check:
- free internal RAM at runtime with Wi-Fi + BLE + display (Home page)
- touch and display after the move to the shared CH422G mutex
- `unscii_8` font on the CAN ID table
- the keyboard pops up on the Wi-Fi password field
- CAN start/stop switches EXIO5 cleanly and logs open on the SD card
- BLE pairing with a phone (numeric comparison is auto-accepted)
- GPS page: works on the board (MOD-GPS on J8, 19200 baud, 8 sats, fix).
  Not yet checked: clock set from GPS, the status line not clipping
- Route page: works on the board (GPX load from SD, map, GPS position on it).
  Not yet checked: autoload at boot, off-route, done/to-go while moving
- SD file manager over Wi-Fi (built, not run): web_svc, http://<IP>/, server
  stack in PSRAM; page JS only read through, not run in a browser yet
- Point 4 (built, not run): join point, rejoin arrow, wrong way, Reverse,
  POIs, sunset; join/reverse/POI/wrong-way host-tested, sun formula checked
  against published Bucharest times
- Ride page (built, not run): stats and GPX writer host-tested with a
  simulated ride; 10-button sidebar with one-line labels; SHORT_CLICKED
  starts, LONG_PRESSED stops
- Turns (built, not run): detection host-tested on synthetic clean and
  jittered tracks; turn box, map ring, screen wake before turns / off route
- Climbs page (built, not run): profile chart, climb detection on real files
  (host-tested on a synthetic track only), live climb info, 9-button sidebar
