# Esp32_monitor

A general-purpose board monitor for the
[Waveshare ESP32-S3-Touch-LCD-7](https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-7):
a touch UI (EEZ Studio + LVGL 8) for exploring what the board can do.

| Page | What it does |
|---|---|
| **Home** | Chip, flash/PSRAM, firmware; uptime, clock, free RAM, die temperature; one line per service |
| **Wi-Fi** | Scan, pick a network, password (on-screen keyboard), connect / disconnect / forget, reconnect at boot, IP / gateway / DNS / RSSI, SNTP clock, the address of the SD card file manager |
| **BLE** | 10 s scan (name, address, RSSI), connect + pair (bonds kept in NVS), list the GATT services, forget bonds |
| **CAN** | Bitrate 125k–1M, listen-only, start/stop, start at boot; frames/s, peak, estimated bus load, error counters; live table of every ID (DLC, data, count, period); **raw log to SD** in candump format |
| **IO** | GPIO6 (Sensor AD header) as analog in (calibrated mV, min/max), digital in (pull-up/-down), digital out or GPS input; I2C scan of the I2C header |
| **GPS** | NMEA GPS on GPIO6: fix, position, altitude, speed, course, DOP, UTC, satellite table with SNR, raw sentences. Sets the clock without Wi-Fi |
| **Climbs** | Elevation profile coloured by gradient (whole route or the next 6 km), climb list (start, length, height, average / steepest %, category), live: elevation and gradient here, climb to the top, next climb, still to climb |
| **Ride** | Big figures: speed, distance, moving time, average, top speed, climbed. Navigation arrow and distance to the next turn (or back to the track / wrong way). Auto-pause when stopped, arrival time for the loaded route, sunset (warns when you would arrive after dark). Records the ride as GPX to `rides/` on the SD card. Short press starts, long press stops |
| **Route** | Pick a GPX file from the SD card (`/gpx`), map of the track with your position, part done, distance to go, off-route warning (> 30 m), ETA. Whole-track or follow view with zoom. **Turns**: arrow + distance over the map, "next / then" in the text, a ring on the map at the turn; the screen switches on before a turn and when going off route. **Join anywhere**: progress counts from where you meet the track; off route an arrow points to it; a wrong-way warning; **Reverse** rides the file end to start. Waypoints are shown as POIs ("Water in 4.3 km"). The last track (and direction) is loaded again at boot |
| **Display** | Brightness, screen-off timeout (the waking touch is swallowed), SD card status / remount, restart |

The status line on every page shows Wi-Fi, CAN (rate, REC), BLE, SD and the clock.

## Hardware (V1.2 schematic)

Almost every GPIO drives the RGB panel. What is left for the outside world:

| Connector | Signal | Notes |
|---|---|---|
| Sensor AD (J8: 3V3 / GND / AD) | **GPIO6** = ADC1 ch5 | Straight to the pin, no divider: **0–3.3 V only** |
| CAN (J6) | TX GPIO20, RX GPIO19 | Shared with native USB, switched by CH422G EXIO5 (high = CAN). 120 Ω jumper fitted by default |
| RS485 (J7) | TX GPIO15, RX GPIO16 | Auto direction. Not used yet |
| I2C (H1) | SDA GPIO8, SCL GPIO9 | Same bus as GT911 touch and CH422G; 3.3/5 V level shifter |
| UART (H3) | GPIO43/44 | Same UART as the CH343 USB port (switch S1). It is the console |
| TF card | MOSI 11, SCK 12, MISO 13, CS = EXIO4 | SPI, FAT32 |

CH422G IO expander: EXIO1 touch reset, EXIO2 backlight enable, EXIO3 LCD reset,
EXIO4 SD CS, EXIO5 USB/CAN select, EXIO6 LCD VDD enable. IO0, IO7 and OC0–3 are
not connected, so there are no spare expander pins.

**Backlight:** the AP3032 boost converter's EN pin is driven by EXIO2 (on/off
only). The PWM network on the BL_EN pad is not fitted, so brightness is
**software dimming** (a black overlay). For real dimming, wire a free GPIO to
the BL_EN pad and set `CONFIG_BACKLIGHT_PWM_GPIO` (menuconfig > Display
brightness).

**CAN and USB share GPIO19/20.** While CAN runs, the native USB-C port is gone.
When CAN is stopped, the pins go back to USB. Flash and monitor through the
**UART** USB-C port (CH343, `/dev/ttyUSB0`); the console is UART0.

**GPS (Olimex MOD-GPS, u-blox 7, UEXT):** listen only, three wires to J8:
UEXT pin 1 VCC -> 3V3, pin 2 GND -> GND, pin 4 -> AD (GPIO6). UEXT names the
pins from the host's side, so pin 4 "RXD" carries the module's NMEA output;
pin 3 (the module's input) stays free. Select "GPS (NMEA in)" as the GPIO6
mode (IO page, or the button on the GPS page). 19200 baud (the MOD-GPS
default) is tried first, other rates are auto-detected. Any NMEA receiver with 3.3 V logic works the same way.

**Bluetooth:** the ESP32-S3 does Bluetooth **LE only**, so classic-only
devices (old headsets, SPP modules) don't show up. Pairing is "just works" or
numeric comparison: the code is shown on the BLE page and accepted
automatically. Devices that want a passkey typed in are not supported yet.

### GPX files

Copy them to `gpx/` on the SD card (`CONFIG_MON_GPX_DIR`, created on the first
listing). Any GPX 1.0/1.1 export works (Strava, Komoot, GPS Visualizer, Garmin,
OsmAnd...): track points are used, or route points if the file has no track.
Waypoints are ignored for now. The map has no background: it is the track,
your position and a scale bar, north up.

Turns are worked out from the track's shape, since a GPX track carries no
instructions: a heading change of at least 35 degrees within about 20 m
(slight < 60, normal < 120, sharp < 160, U-turn). Gentle bends spread over
a long distance are not turns; going straight on at a junction gives no
instruction, and a sharp bend in the road is reported as a turn. With a
screen timeout set (Display page), the screen comes on 25 s (at least
150 m) before each turn and when you go off route, shows the Route page,
and stays on until the turn is passed.

**Starting elsewhere:** you can start anywhere. The first time you are on
the track (within 30 m) becomes the join point: "done" and the percentage
count from there, the map draws the part done from there, and turns,
climbs, POIs and the profile are all "ahead of you". Before that, and
whenever you are off route, the box over the map points to the nearest
point of the track (relative to your direction when moving, north up when
standing). Riding against the track's direction for 5 s shows "Wrong way!"
(and wakes the screen); to ride a file backwards press **Reverse**: turns,
climbs and the profile are worked out again for that direction. A new
ride (Ride page) sets a new join point.

**Waypoints** (`<wpt>` with a `<name>`, up to 64) are drawn as yellow dots,
named when zoomed in (8 m/px or closer) and for the next one; those within
300 m of the track get a distance along it ("Water in 4.3 km").

**SD card over Wi-Fi:** with Wi-Fi connected, open `http://<board IP>/`
in a browser on the same network (the Wi-Fi page shows the address). It
lists `gpx/`, `rides/` and `canlog/` with sizes and free space; click a
file to download it, delete files, and upload GPX files into `gpx/`
(several at once; the Route page's list updates by itself). There is no
password: anyone on your network can use it, so only join networks you
trust.

**Clock / time zone:** times on screen (clock, arrival, sunset) use
`CONFIG_MON_TZ` (menuconfig > Board monitor configuration), a POSIX TZ
string; the default is UTC. For example `CET-1CEST,M3.5.0,M10.5.0/3`
(Central Europe) or `EET-2EEST,M3.5.0/3,M10.5.0/4` (Eastern Europe).

Ride recordings go to `rides/YYYYMMDD_HHMMSS.gpx` (`CONFIG_MON_RIDE_DIR`):
one point per second while moving, with elevation and UTC time, ready for
Strava / Komoot upload. The file is written every 10 s and always ends with
valid closing tags, so a power cut loses at most the last 10 s. Moving
means above 4 km/h (stopped below 2 km/h); distance only counts while
moving; climbed uses the GPS altitude with a 4 m threshold, so it reads
lower than the route's figure from the GPX elevation.

Elevation comes from the file's `<ele>` (not the GPS altitude), resampled
every 10 m and smoothed over +-50 m. A climb is at least 300 m long, 15 m
high and 3 % on average; a dip of less than 10 m does not end it, flat
run-ins and run-outs are cut off. The category follows Strava's length x
gradient score (4: 8000, 3: 16000, 2: 32000, 1: 64000, HC: 80000).
Gradient colours: green < 3 %, yellow < 6 %, orange < 9 %, red < 12 %,
purple steeper, grey downhill.

## Architecture

```
 wifi_svc ─┐
 ble_svc  ─┤  status structs (thread safe, polled)   ┌──────────────┐
 canmon   ─┼────────────────────────────────────────▶│ hmi/bridge   │──▶ EEZ variables ──▶ screens
 io_svc   ─┤  ◀──── non-blocking commands ───────────│ (LVGL task)  │◀── native actions ◀── buttons
 settings ─┘                                         └──────────────┘
 can_if ─▶ canmon ─▶ ID table, stats, /sdcard/canlog/*.log
 board: LCD, touch, CH422G, SD card, LVGL port (from FP_Dash)
```

| Component | Role |
|---|---|
| `components/board` | LCD, GT911 touch, CH422G (with a mutex), SD card, LVGL port (mode 4: render in internal RAM). Taken from FP_Dash |
| `components/can_if` | TWAI driver (from FP_Dash) plus runtime start/stop, bitrate change and error counters |
| `components/canmon` | Bus monitor task: ID table, rates, bus load, candump logger with a 64 KB PSRAM buffer |
| `components/wifi_svc` | Station: scan, connect, retries, stored network, SNTP |
| `components/ble_svc` | NimBLE central: scan, connect, pair/bond, GATT service discovery |
| `components/io_svc` | GPIO6 modes, ADC with eFuse calibration, die temperature, I2C scan |
| `components/gpx_svc` | GPX files on the SD card: listing, streaming parser (track, else route; up to 100k points in PSRAM), matching the GPS position to the track |
| `components/ride_svc` | Ride statistics (moving time, distance, speeds, climbed) and GPX recording to the SD card |
| `components/web_svc` | HTTP file manager for the SD card (list, download, upload into gpx/, delete) |
| `components/gps_svc` | NMEA receiver on UART1 RX: baud auto-detect, RMC/GGA/GSA/GSV, satellite table, sets the clock |
| `components/settings` | User choices in one NVS blob, saved 2 s after the last change |
| `components/hmi` | EEZ project, generated code in `src/ui`, hand-written `bridge/` |

### CAN log format

`/sdcard/canlog/YYYYMMDD_HHMMSS.log` when the clock is set over Wi-Fi, else
`can_NNNN.log` with seconds since boot. One line per frame, candump format:

```
(1731488400.123000) can0 1A0#0102030405060708
```

This works with can-utils (`canplayer`, `log2asc`), SavvyCAN and python-can.
Data is written in 32 KB chunks and fsync'ed every 2 s
(`CONFIG_MON_CANLOG_SYNC_MS`), so a power cut costs at most that much traffic.

## Changing the UI (EEZ Studio)

The UI source is `components/hmi/Esp32_monitor.eez-project` (EEZ Studio 0.29,
LVGL 8.3, EEZ Flow, dark theme).

1. Open it in EEZ Studio: `~/tools/EEZ-Studio-0.29.0.AppImage --no-sandbox`.
   Move, restyle and add widgets as you like.
2. Build in EEZ Studio, or run `tools/eez-build.sh` (headless; needs a desktop session).
3. `idf.py build flash monitor`

The firmware only depends on **names**:
- **Global variables**: the bridge looks them up by name at start-up. A
  variable you delete is ignored and logged once. Two-way bound inputs
  (checkbox checked state, slider value, dropdown selected, text area text)
  are read back by the bridge.
- **Native actions** (`nav_*`, `wifi_*`, `ble_*`, `can_*`, `io_*`, `i2c_scan`,
  `sd_remount`, `screen_off`, `restart`): each has a C function in
  `bridge/actions.c`. Adding an action in EEZ needs one there too, or the link fails.
- **Screens** `home`, `wifi`, `ble`, `can`, `io`, `gps`, `route`, `profile`, `ride`, `display`: the `nav_<name>`
  actions open them, and the last page is restored at power-up.
- **Identifiers** `can_id_table`, `gps_sat_table`, `gps_raw_table`: these
  labels get LVGL's monospace font (`unscii_8`) from the
  bridge. EEZ 0.29 only offers Montserrat fonts.
- **Identifiers** `route_map`, `profile_chart`: empty panels; the bridge
  draws the map / the elevation profile into them. Move or resize them
  freely, keep them empty. `profile_climb_table` gets a monospace font.

Every text area automatically gets the on-screen keyboard.

`tools/eez_scaffold.py` generated the first version of the project from
`tools/eez_templates.json`, which holds widget JSON as EEZ Studio writes it.
It refuses to overwrite the project. `--force` regenerates it from scratch and
**throws away every change made in EEZ Studio**.

The full variable list is in `VARIABLES` in `tools/eez_scaffold.py`.
`tools/eez_add_page.py <page>` adds a page defined in the scaffold to the
existing project in place (variables, actions, sidebar button on every page,
the screen); it does nothing when the screen exists. The GPS and Route pages
were added that way.

## Build

```sh
. ~/.espressif/tools/activate_idf_v6.1.sh
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

`sdkconfig` is local and not committed. Defaults are in `sdkconfig.defaults`,
and project options are under menuconfig > *Board monitor configuration*:
time zone, NTP server, log directory, sync period and ID table size.

The vendor's original example (RGB LCD demo, does not build on IDF 6.x) is kept
in `reference/vendor_example/`.

## Next ideas

- **USB-C:** with CAN stopped, GPIO19/20 are the native USB OTG port. Device
  mode (TinyUSB CDC / MSC to read the CAN logs from a PC) works electrically.
  Host mode needs 5 V on VBUS for the attached device, and the board's USB-C
  port is a power *input*: check the schematic or add a powered hub before trying.
- RS485 page (GPIO15/16): a terminal or Modbus RTU master.
- CAN transmit (single frame / periodic) and ID filters on the CAN page.
- BLE: read characteristics (battery level, device info), passkey entry.
- Hardware backlight PWM (a wire to BL_EN).
