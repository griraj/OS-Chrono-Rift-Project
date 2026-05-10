# ─────────────────────────────────────────────────────────────────────────────
# Chrono Rift — Native Linux Makefile (no Docker)
# Targets: arbiter_bin  hip_bin  asp_bin
#
# Dependencies (Ubuntu/Debian):
#   sudo apt-get install build-essential libsfml-dev
#
# Build:  make
# Run:    ./arbiter_bin   (the launcher prompts for seed + party size)
# Clean:  make clean
# ─────────────────────────────────────────────────────────────────────────────

CXX      := g++
CXXFLAGS := -Wall -Wextra -std=c++17 -pthread -O2

# SFML + POSIX real-time library
SFML_LIBS := -lsfml-graphics -lsfml-window -lsfml-system
LIBS      := $(SFML_LIBS) -lrt
PWD       := $(shell pwd)

TARGETS := arbiter_bin hip_bin asp_bin

.PHONY: all clean run install-deps docker-build docker-run

all: $(TARGETS)
	@echo "──────────────────────────────────────────"
	@echo "  Build complete."
	@echo "  Run with:  ./arbiter_bin"
	@echo "──────────────────────────────────────────"

# ── Arbiter (includes SFML renderer + deadlock monitor + launcher) ────────────
arbiter_bin: arbiter/arbiter.cpp arbiter/inventory.cpp arbiter/main_launcher.cpp \
             arbiter/inventory.h arbiter/shared_state.h arbiter/sprite_draw.inl
	$(CXX) $(CXXFLAGS) \
	    arbiter/arbiter.cpp \
	    arbiter/inventory.cpp \
	    arbiter/main_launcher.cpp \
	    -o arbiter_bin $(LIBS)

# ── Human Interface Process ───────────────────────────────────────────────────
hip_bin: hip/hip.cpp hip/inventory.cpp hip/inventory.h hip/shared_state.h
	$(CXX) $(CXXFLAGS) \
	    hip/hip.cpp \
	    hip/inventory.cpp \
	    -o hip_bin $(SFML_LIBS) -lrt -pthread

# ── Automated Strategic Process ───────────────────────────────────────────────
asp_bin: asp/asp.cpp asp/inventory.h asp/shared_state.h
	$(CXX) $(CXXFLAGS) \
	    asp/asp.cpp \
	    asp/inventory.cpp \
	    -o asp_bin -lrt -pthread

# ── Install build dependencies (requires sudo) ────────────────────────────────
install-deps:
	sudo apt-get update
	sudo apt-get install -y build-essential libsfml-dev fonts-dejavu-core

# ── Cleanup ───────────────────────────────────────────────────────────────────
docker-build:
	docker build -t chrono_rift .

docker-run:
	docker run --rm \
		-e DISPLAY=$(DISPLAY) \
		-v /tmp/.X11-unix:/tmp/.X11-unix \
		--workdir /app \
		--network host \
		chrono_rift ./arbiter_bin

clean:
	rm -f arbiter_bin hip_bin asp_bin
	@echo "Cleaned build artifacts."
