# Chrono Rift — Native Linux Build Guide
## CS 2006 Operating Systems — Spring 2026

---

## Overview

Chrono Rift is a multi-process, multi-threaded turn-based tactical game.
This version runs **natively on Linux** (Ubuntu 22.04+) with no Docker required.

### Key OS concepts preserved

| Concept | Implementation |
|---|---|
| Multi-process architecture | `arbiter_bin` forks `hip_bin` and `asp_bin` |
| Thread-per-NPC | Each enemy gets its own `pthread` in ASP |
| Thread-per-Player | Each player gets its own `pthread` in HIP |
| Shared memory IPC only | POSIX `shm_open` / `mmap`; **zero pipes** |
| Semaphore synchronisation | `sem_init(pshared=1)` for cross-process mutexes |
| Signal-based stun | `SIGUSR1` → async handler sleeps 3 s |
| Signal-based Ultimate pause | Arbiter sends `SIGSTOP`/`SIGCONT` to ASP; `SIGALRM` resumes after 10 s |
| Deadlock detection | Background thread monitors artifact table; forces release |
| SFML rendering thread | Separate `pthread` owns the `sf::RenderWindow`; reads shared memory at 10 fps |
| Inventory allocator | Contiguous-fit with LTS swap-out |

---

## Prerequisites

Install dependencies (Ubuntu 22.04+):

```bash
sudo apt-get update
sudo apt-get install -y build-essential libsfml-dev fonts-dejavu-core
```

Or use the Makefile shortcut:

```bash
make install-deps
```

---

## Build

```bash
cd chrono_rift
make
```

This produces three executables in the project root:

```
arbiter_bin   ← Game Arbiter + SFML renderer + deadlock monitor
hip_bin       ← Human Interface Process (player input threads)
asp_bin       ← Automated Strategic Process (NPC AI threads)
```

---

## Run

**Always launch via the arbiter — it spawns hip and asp automatically:**

```bash
./arbiter_bin
```

You will see:
1. **Splash screen** — animated CHRONO RIFT title (4-5s, press any key to skip)
2. **Launcher** — green-shaded rectangle card to enter Roll Number and Party Size
3. **Game window** — SFML HUD with pixel-art player/enemy sprites, HP/stamina bars, artifact table, action log

**Player input** is done via the HIP window (click actions and targets).

---

## Controls (HIP window)

On each player's turn a GUI panel appears:

```
Strike          direct damage to enemy
Exhaust         drain enemy stamina
Use Weapon      attack with inventory weapon
Swap In         retrieve from Long-Term Storage (costs turn)
Heal            restore 10% HP
Skip            50% stamina refund
ULTIMATE        requires Solar Core + Lunar Blade in inventory
Quit Game       sends SIGTERM to arbiter
```

Click an action, then click a target enemy when prompted. Press **P** for pause menu.

---

## Win / Lose conditions

| Condition | Result |
|---|---|
| Kill 10 enemies total | **VICTORY** |
| All player characters die | **DEFEAT** |
| Player chooses Quit | **QUIT** (sends `SIGTERM` to arbiter) |

---

## Clean build

```bash
make clean
```

---

## Project structure

```
chrono_rift/
├── Makefile
├── README.md
├── arbiter/
│   ├── arbiter.cpp        ← Arbiter + SFML renderer + deadlock monitor
│   ├── main_launcher.cpp  ← Splash + green launcher UI + entry point
│   ├── sprite_draw.inl    ← Pixel-art sprite helpers (included by arbiter.cpp)
│   ├── inventory.h
│   ├── inventory.cpp
│   └── shared_state.h
├── hip/
│   ├── hip.cpp            ← Human Interface Process
│   ├── inventory.h
│   ├── inventory.cpp
│   └── shared_state.h
└── asp/
    ├── asp.cpp            ← Automated Strategic Process
    ├── inventory.h
    ├── inventory.cpp
    └── shared_state.h
```

---

## Sprites

Each entity in the HUD now displays a 20×20 pixel-art sprite:

| Sprite | Meaning |
|---|---|
| Gold knight helmet | Player character |
| Orange-red horned skull | Enemy |
| Purple lightning bolt | Stunned entity |
| Grey cross | Dead entity |

---

## Troubleshooting

| Problem | Fix |
|---|---|
| `libsfml-graphics.so not found` | `sudo apt-get install libsfml-dev` |
| No monospace font, garbled UI | `sudo apt-get install fonts-dejavu-core` |
| `shm_open` permission denied | Run `sudo rm /dev/shm/chrono_rift_shm` to clean stale segments |
| Window doesn't open | Ensure a display is available (`$DISPLAY` set) |
| `hip_bin` / `asp_bin` not found | Run `./arbiter_bin` from the project root directory |
