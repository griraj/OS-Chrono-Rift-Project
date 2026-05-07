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

You will be prompted for:
1. **Roll Number** — used as the RNG seed (affects HP, enemies, etc.)
2. **Party size** — 1 to 4 human-controlled characters

The SFML window opens showing:
- Real-time HP bars (green = players, red = enemies)
- Stamina bars (cyan)
- Artifact ownership table
- Colour-coded action log

**Player input** is typed in the **terminal** (stdin), not the GUI window.

---

## Controls (terminal)

On each player's turn the terminal shows a HUD and a numbered menu:

```
1) Strike          (direct damage)
2) Exhaust         (drain enemy stamina)
3) Use Weapon      (attack with inventory weapon)
4) Swap In         (retrieve from Long-Term Storage, costs turn)
5) Heal            (restore 10% HP)
6) Skip            (50% stamina refund)
7) ULTIMATE ABILITY (requires Solar Core + Lunar Blade in inventory)
0) Quit Game
```

Enter the number and follow the prompts for target selection.

---

## Win / Lose conditions

| Condition | Result |
|---|---|
| Kill 10 enemies total | **VICTORY** |
| All player characters die | **DEFEAT** |
| Player chooses option 0 | **QUIT** (sends `SIGTERM` to arbiter) |

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
│   ├── arbiter.cpp       
│   ├── main_launcher.cpp  
│   ├── inventory.h
│   ├── inventory.cpp
│   └── shared_state.h    
├── hip/
│   ├── hip.cpp            
│   ├── inventory.h
│   ├── inventory.cpp
│   └── shared_state.h
└── asp/
    ├── asp.cpp            
    ├── inventory.h
    ├── inventory.cpp
    └── shared_state.h
```

---

## SFML rendering thread details

The renderer lives in `render_thread_fn()` inside `arbiter.cpp`.

- The `sf::RenderWindow` is created **inside the rendering thread** (SFML
  requires the window and its OpenGL context to stay on one thread).
- The game-logic threads **never call any SFML function**.
- Every ~100 ms the renderer acquires `state_mutex`, copies the full
  `SharedState` into a local snapshot, then releases the lock before drawing.
- This ensures the scheduler is never blocked by rendering work.
- Closing the window sets `g_quit = 1` and triggers a clean shutdown.

---

## Troubleshooting

| Problem | Fix |
|---|---|
| `libsfml-graphics.so not found` | `sudo apt-get install libsfml-dev` |
| No monospace font, garbled UI | `sudo apt-get install fonts-dejavu-core` |
| `shm_open` permission denied | Run `sudo rm /dev/shm/chrono_rift_shm` to clean stale segments |
| Window doesn't open | Ensure a display is available (`$DISPLAY` set, or run with `--display`) |
| `hip_bin` / `asp_bin` not found | Run `./arbiter_bin` from the project root directory |
