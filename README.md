# hw-linux — the board straddle for a simulated station

A board straddle like any other, except that its board is a Linux process. It
sets `target: linux`, so a build that adds it compiles the firmware against
**ESP-IDF's own Linux host target** and produces an ELF instead of a flashable
image. One process is one station.

This exists for **simulation**: a testbed where a handful of stations run on
one machine and talk to each other over a virtual radio. That testbed is
described in [`reticulous/sim/`](../reticulous/sim/README.md) — how to run it,
what it is made of, and why it is built this way.

## What this is not

It is not an emulator. There is no Xtensa, no instruction emulation and
nothing of the chip: ESP-IDF's Linux target is a host port of FreeRTOS — one
pthread per task — plus a subset of IDF's components, so the same firmware
sources compile with host gcc and run at native speed.

It is also **not a way to run spangap, or anything built on it, on a Linux
box**. A station on this board has:

- no radio — the SX1262 is a model that only ever talks to the virtual ether,
  so stations reach each other and nothing else;
- no WiFi and no lwIP — sockets are the host's, and a station binds one
  loopback address;
- no flash — the state store is a directory and NVS is a file under `/tmp`;
- no OTA, power management, USB console, mDNS or NTP.

For Reticulum on a real Linux machine, use the upstream Python stack —
`install-reticulum` in the build container installs it.

## What the straddle supplies

A chip supplies four things the host target does not, and each is here:

| | |
|---|---|
| `esp-idf/src/hwlinux.cpp` | the station's identity and directory, from the environment below, and `esp_efuse_mac_get_default` over the node id |
| `esp-idf/components/driver/` | the GPIO shim — a pin table whose one rule is that a level-triggered pin fires the instant its interrupt is enabled while the line is asserted. Also the two SPI type names the firmware's declarations mention |
| `esp-idf/components/esp_timer/` | esp_timer over `CLOCK_MONOTONIC` and one task; ESP-IDF's own registers headers only on this target |
| `esp-idf/src/detect.cpp` | the board's self-assertion, which is unconditional — there is nothing to probe |

Both sub-components take the name of an ESP-IDF component and override it,
which works because the buildable puts every staged straddle's `components/`
directory on `EXTRA_COMPONENT_DIRS`, later in the search order than IDF's own.

**The radio.** This straddle declares the radio's pins and owns the GPIO shim
the modem drives its interrupt line through; the chip model itself belongs to
the interface that drives it, in
[`iface-lora`](../iface-lora/README.md)'s `src/host/`. A feature straddle may
not depend on a board straddle, so the model cannot live here.

## The environment a station is given

Read once, at the first call:

| Variable | What it is |
|---|---|
| `SPANGAP_NODE_ID` | a small integer. The last byte of the station's MAC, so every station on one host is a distinct device |
| `SPANGAP_NODE_DIR` | the station's directory. Created, with `state/` under it; the process `chdir()`s there, which is what makes the platform's filesystem roots resolve inside it |
| `SPANGAP_BIND_ADDR` | the address every listener binds, `127.0.0.1<id>` by default, so stations keep the canonical port numbers instead of offsetting them |
| `SPANGAP_ETHER` | `host:port` of the virtual ether. Absent: the radio transmits into nothing |
| `SPANGAP_FIXED_DIR` | the build's `data_merged`, linked into the station directory as `fixed` |

A station's ports are its own: the web UI on 80 and 443, the TCP CLI on 8081,
and whatever else the build registers. Its console is its stdin and stdout.

## Building

The launcher builds nothing; point it at an ELF built with this board. The
build command, the staged set and the first-run answers a station needs are in
[`reticulous/sim/README.md`](../reticulous/sim/README.md).
