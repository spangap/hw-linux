# hw-linux — the board straddle for a station that is a Linux process

A board straddle like any other, except that its board is a Linux process. It
sets `target: linux`, so a build that adds it compiles the firmware against
**ESP-IDF's own Linux host target** and produces an ELF instead of a flashable
image. One process is one station.

This exists for **simulation**: many stations on one machine, each its own
process on its own loopback address, talking to each other over a virtual
radio. The simulation itself — the medium, the chip model, the program that
starts and watches the stations — lives outside this straddle; what is here is
what any such station needs from its board.

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

A chip supplies things the host target does not, and each is here:

| | |
|---|---|
| `esp-idf/src/hwlinux.cpp` | the station's identity and directory, from the environment below, and `esp_efuse_mac_get_default` over the node id; ESP-IDF's log goes to stdout with `write(2)` from before `main()` until the platform's logger takes it, because stdio's lock can be held by a task a signal switched out |
| `esp-idf/src/fdwait.cpp` | an interrupt controller for file descriptors: `select()` and `hwLinuxWait()` block a task until a descriptor is ready (below) |
| `esp-idf/src/tick.cpp` | the FreeRTOS tick, suppressed while every task is blocked: tickless idle, on the host's clock or on one another component keeps (below) |
| `esp-idf/components/driver/` | the GPIO shim — a pin table whose one rule is that a level-triggered pin fires the instant its interrupt is enabled while the line is asserted. Also the two SPI type names the firmware's declarations mention |
| `esp-idf/components/esp_timer/` | esp_timer over one task, on `CLOCK_MONOTONIC` or on a clock another component keeps (below); ESP-IDF's own registers headers only on this target |
| `esp-idf/src/detect.cpp` | the board's self-assertion, which is unconditional — there is nothing to probe |

Both sub-components take the name of an ESP-IDF component and override it,
which works because the buildable puts every staged straddle's `components/`
directory on `EXTRA_COMPONENT_DIRS`, later in the search order than IDF's own.

**The radio.** This straddle declares the radio's pins and owns the GPIO shim
the modem drives its interrupt line through; the chip model itself comes in
with the interface that drives it, [`iface-lora`](../iface-lora/README.md). A
feature straddle may not depend on a board straddle, so the model cannot come
from here.

## Waiting on a descriptor

```
task            select(fds, timeout)   arm the fds (epoll, one-shot), block on a notification
epoll thread    epoll_wait → ready     mark the waiting tasks, kill(SIGUSR2)
SIGUSR2         on the running task's thread, as the tick is: notify FromISR, switch if due
task            woken                  disarm, report the ready sets
```

On this port a task that blocks in a host system call is, to the scheduler,
still running, and nothing below it gets the CPU until the call returns. So a
task may only wait on a FreeRTOS primitive, and the host target's own answer
for sockets — IDF interposes `select()` as a zero-timeout check and a sleep of
up to ten ticks, in a loop — is a poll at tick rate in every task that waits
on one.

This board replaces that with an interrupt. One ordinary pthread, outside
FreeRTOS, blocks in `epoll_wait` over whatever descriptors tasks are waiting
on; it never calls a kernel API, it only records which tasks to wake and
raises `SIGUSR2`. The signal lands where the tick's `SIGALRM` does — on the
thread of the task that is running, since every other task thread has
signals blocked — and its handler notifies the waiting tasks with the
`FromISR` calls and switches to one of them if it outranks the running task,
exactly as the tick handler does. A task in a critical section has signals
blocked, so the handler waits for it to leave, as an interrupt would.

A suspended scheduler (a task inside `vTaskSuspendAll`) is not a stopped one,
and the handler does its work then too: the `FromISR` calls queue the woken
task and the switch happens when the scheduler resumes. The signal is the
only thing that delivers the pending wakeups, so a handler that returned
without them would lose them for good. Only before the scheduler has started
does it leave them pending.

Before the scheduler starts there are no tasks to switch, and both waits below
are the host's own `pselect()`. A task that finds all 64 waiter slots taken
falls back to the port's polling: a zero-timeout check and a one-tick delay,
in a loop.

Two waits come of it:

- **`select()`**, for every caller in the image, with the POSIX meaning. It is
  wrapped at link time (`-Wl,--wrap=select`), so callers need no change, and
  it blocks the task on notification index 1
  (`CONFIG_FREERTOS_TASK_NOTIFICATION_ARRAY_ENTRIES=2`), which nothing else
  uses.
- **`hwLinuxWait()`**, `select()` that also returns when the task is notified
  on index 0, without taking the notification's count. A task that serves
  both descriptors and inter-task messages — a console, a socket relay —
  sleeps in it until either arrives. Callers outside this straddle reach it
  through a weak declaration and keep their chip behaviour when it is absent.

## The tick

```
idle task       no task due for ≥ 2 ticks       portSUPPRESS_TICKS_AND_SLEEP(n), scheduler suspended
suppress        interrupts off: stop the tick, confirm nothing became ready
suppress        ppoll(until tick n, signals open)   the sleep; SIGUSR2 or the timeout ends it
suppress        step the ticks that passed (vTaskStepTick), restart the tick on its phase
```

The kernel is built **tickless** (FreeRTOS's `configUSE_TICKLESS_IDLE`), so a
station whose tasks are all blocked costs nothing until the first of them is
due or an interrupt arrives, the way a chip's tickless idle does. ESP-IDF
offers tickless idle only with power management, which this target does not
build, so `esp-idf/CMakeLists.txt` sets the two values its `FreeRTOSConfig.h`
reads (`CONFIG_FREERTOS_USE_TICKLESS_IDLE=1`,
`CONFIG_FREERTOS_IDLE_TIME_BEFORE_SLEEP=2`) as compile definitions of the whole
image, and gives the kernel's sources `src/suppress.h` ahead of everything, the
port's `portSUPPRESS_TICKS_AND_SLEEP`. Nothing under `/opt/esp/idf` changes.

On the host's clock the tick is the port's 100 Hz `setitimer`, and idle stops
it, sleeps in `ppoll()` until the tick the first task is due at, and restarts
it on the phase it had. The sleep has signals blocked up to the `ppoll()` that
opens them, so an interrupt that arrives while idle decides to sleep ends the
sleep instead of being lost: the descriptor controller's `SIGUSR2` is one,
and a tick that was already pending cancels the sleep altogether.

The kernel suppresses the tick only when no task is due for two ticks or more,
and calls the port's own idle hook on every pass before deciding; that hook
(`port_idf.c`) sleeps 15 ms. It is replaced (`-Wl,--wrap`) by one that returns
at once on the first pass after the suppress, and on a pass after one the
kernel kept — a task due at the very next tick — sleeps until an interrupt,
the tick included. The idle task never spins and never sleeps on a timer of
its own.

A quiet station's tasks all block, and nothing on this board runs at tick rate
while they do. What still wakes a quiet station is its own firmware's timers.

## The station's clock

```
board onStart        hwLinuxClockStart()          the clock comes up, before anything waits;
                                                   nonzero: the tick follows it
esp_timer            hwLinuxClockUs()             every esp_timer_get_time()
esp_timer            hwLinuxClockWake(us)         the earliest expiry moved (INT64_MAX: none)
board                hwLinuxClockTickAt(us)       the next tick a task waits for (INT64_MAX: none)
idle task            hwLinuxClockIdle()           every task is blocked until the earlier of
                                                   those two, or an interrupt
clock                hwLinuxClockDue()            the expiry is reached: the timer task runs it
clock                hwLinuxClockMoved()          the clock moved: the tick count follows, at once
```

By default a station runs on the host's clocks: esp_timer is
`CLOCK_MONOTONIC` from its first reading, and the tick is the port's
`setitimer`. Another component in the image may keep the station's clock
instead, by defining the `hwLinuxClock*` functions in
[`hwlinux.h`](esp-idf/include/hwlinux.h). The board declares them weak and
calls whichever are defined, so an image without them runs on the host's
clocks unchanged.

A clock that is not the host's cannot be slept on by the timer task's own
wait, so esp_timer tells the clock where its earliest expiry is whenever that
changes, and the clock calls `hwLinuxClockDue()` when it reaches that instant;
the timer task then runs whatever is due, as it would on waking.

The tick follows such a clock too. When `hwLinuxClockStart()` says the clock is
not the host's, the board stops the port's `setitimer` for good, and from then
on tick n falls at a whole multiple of the tick period on the station's clock
(`hwLinuxClockUs`): every time the clock moves it calls `hwLinuxClockMoved()`,
from the task that moved it, and the count is brought up to it there
(`xTaskCatchUpTicks`), before anything due at the new instant runs. So a clock
that jumps over a long quiet stretch lands with exactly the ticks it crossed,
and the tasks due in them wake in order. The board tells the clock the next
tick anybody waits for: while a task runs that is the next tick, and while
every task is blocked it is the tick the first of them is due at. Idle says so
with `hwLinuxClockIdle()` and sleeps until an interrupt; the clock's own
message arriving on a descriptor is one.

FreeRTOS's delays and the timeouts of `select()` and `hwLinuxWait()` are in
ticks, so they follow whichever clock the tick does. The C library's own
clocks and waits are the host's; a clock that means to move them too answers
the process's `clock_gettime` and timed waits, for instance from a preloaded
library.

## The environment a station is given

Read once, at the first call:

| Variable | What it is |
|---|---|
| `SPANGAP_NODE_ID` | an integer from 1 to 65535. The last two bytes of the station's MAC, so every station on one host is a distinct device |
| `SPANGAP_NODE_DIR` | the station's directory. Created, with `state/` under it; the process `chdir()`s there, which is what makes the platform's filesystem roots resolve inside it |
| `SPANGAP_BIND_ADDR` | the address every listener binds, `127.0.0.1<id>` by default, so stations keep the canonical port numbers instead of offsetting them |
| `SPANGAP_ETHER` | `host:port` of the virtual ether. Absent: the radio transmits into nothing |
| `SPANGAP_FIXED_DIR` | the build's `data_merged`, linked into the station directory as `fixed` |

A station's ports are its own: the web UI on 80 and 443, the TCP CLI on 8081
once `s.net.cli_port` opens it (closed by default, as on a chip), and whatever
else the build registers. Its console is its stdin and stdout, and carries
framed RPC exactly as a board's USB console does.

## Building

Add the board to any buildable whose straddles build for `linux`:

```sh
spangap build <project> --with spangap/hw-linux -x <each straddle that does not>
```

The result is `esp-idf/build.linux/<project>.elf` in the buildable, with its
`/fixed` tree in `esp-idf/build.linux/data_merged/`. Nothing launches it: the
program that runs stations sets the environment above and starts the ELF.
