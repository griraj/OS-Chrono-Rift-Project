/*
 * asp.cpp  –  Automated Strategic Process (main entry point → produces 'asp' executable)
 *
 * Responsibilities:
 *  - Opens the POSIX shared memory segment (no creation)
 *  - Creates ONE pthread per enemy NPC  (concurrent, not sequential)
 *  - Creates ONE dispatcher thread watching gs->turn_sem
 *  - Each NPC thread independently decides its action (true concurrency)
 *  - NPC AI posts action to shared memory mailbox, signals arbiter via action_sem
 *  - Handles SIGUSR1 (stun): affected NPC skips its turn
 *  - Handles SIGSTOP / SIGCONT from arbiter for Ultimate pause window
 *    (these are OS-level signals — no flag or pipe coordination needed)
 *  - Handles SIGTERM for graceful shutdown
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <semaphore.h>
#include <time.h>
#include <errno.h>
#include <climits>

#include "shared_state.h"

/* ── Globals ─────────────────────────────────────────────────────── */
static SharedState* gs            = nullptr;
static int          g_num_enemies = 0;
static int          g_num_players = 0;
static volatile sig_atomic_t g_quit      = 0;
static volatile sig_atomic_t g_stun_flag = 0;

/* Per-NPC wake semaphores */
static sem_t npc_sem[MAX_ENEMIES];

/* ── Signal handlers ─────────────────────────────────────────────── */
static void sig_term(int) { g_quit = 1; }

/*
 * SIGUSR1 — Stun.
 * Asynchronous, non-blocking: handler fires mid-execution, sleeps 3 s,
 * then clears the stun state. No flag polling in the NPC's main loop.
 */
static void sig_stun(int)
{
    g_stun_flag = 1;
    int ct = gs->current_turn;
    if (ct >= gs->num_players && ct < gs->total_entities)
        gs->entities[ct].stunned = true;

    sleep(STUN_DURATION);

    if (ct >= gs->num_players && ct < gs->total_entities) {
        gs->entities[ct].stunned = false;
        gs->entities[ct].stamina = 0;
    }
    g_stun_flag = 0;
}

/* SIGSTOP / SIGCONT are handled by the OS — not overridden.
 * The arbiter sends SIGSTOP to freeze this entire process during Ultimate,
 * and SIGCONT to resume it. No handler needed. */

/* ── Open shared memory ──────────────────────────────────────────── */
static SharedState* shm_open_existing()
{
    int fd = -1;
    for (int i = 0; i < 30; ++i) {
        fd = shm_open(SHM_NAME, O_RDWR, 0666);
        if (fd >= 0) break;
        usleep(100000);
    }
    if (fd < 0) { perror("asp: shm_open"); exit(1); }
    void* p = mmap(nullptr, sizeof(SharedState),
                   PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { perror("asp: mmap"); exit(1); }
    close(fd);
    return static_cast<SharedState*>(p);
}

/* ── NPC AI decision ─────────────────────────────────────────────── */
/*
 * Each NPC thread calls this independently and concurrently.
 * AI logic:
 *   - 80 %: Strike the alive player with the lowest current HP
 *   - 20 %: Skip
 *
 * Uses a deterministic per-NPC RNG derived from entity index + stamina
 * so different NPCs make different decisions.
 */
static Action npc_decide(int entity_idx)
{
    Action act{};

    /* Find lowest-HP alive player */
    int best = -1, best_hp = INT_MAX;
    for (int i = 0; i < gs->num_players; ++i) {
        const Entity& p = gs->entities[i];
        if (p.alive && p.hp < best_hp) { best = i; best_hp = p.hp; }
    }
    if (best < 0) { act.type = ActionType::SKIP; return act; }

    /* Per-NPC entropy: mix entity index with current stamina bits */
    unsigned r = static_cast<unsigned>(entity_idx) * 1664525u + 1013904223u;
    r ^= static_cast<unsigned>(gs->entities[entity_idx].stamina * 100.0f);

    if ((r & 0xF) < 13) { /* ~80 % */
        act.type       = ActionType::STRIKE;
        act.target_idx = best;
    } else {
        act.type = ActionType::SKIP;
    }
    return act;
}

/* ── NPC thread ──────────────────────────────────────────────────── */
struct NpcArg { int local_idx; int entity_idx; };

static void* npc_thread(void* arg_)
{
    NpcArg* arg    = static_cast<NpcArg*>(arg_);
    int local      = arg->local_idx;
    int eidx       = arg->entity_idx;
    delete arg;

    /* Register PID for signal delivery */
    gs->entities[eidx].pid = getpid();

    while (!g_quit) {
        sem_wait(&npc_sem[local]);
        if (g_quit) break;

        /* Verify turn */
        bool our_turn = (gs->current_turn == eidx &&
                         gs->turn_ready &&
                         gs->entities[eidx].alive);
        if (!our_turn) continue;

        /* Stunned: skip */
        if (gs->entities[eidx].stunned || g_stun_flag) {
            Action skip{ActionType::SKIP, -1, -1, WPN_NONE};
            sem_wait(&gs->state_mutex);
            gs->entities[eidx].pending_action = skip;
            gs->entities[eidx].action_ready   = true;
            sem_post(&gs->state_mutex);
            sem_post(&gs->action_sem);
            continue;
        }

        /* Each NPC decides concurrently in its own thread */
        Action act = npc_decide(eidx);

        sem_wait(&gs->state_mutex);
        gs->entities[eidx].pending_action = act;
        gs->entities[eidx].action_ready   = true;
        sem_post(&gs->state_mutex);

        sem_post(&gs->action_sem);

        /* Wait for arbiter to acknowledge */
        while (!g_quit && !gs->entities[eidx].action_done)
            usleep(5000);
    }
    return nullptr;
}

/* ── Turn dispatcher thread ──────────────────────────────────────── */
static void* dispatcher(void*)
{
    while (!g_quit) {
        sem_wait(&gs->turn_sem);
        if (g_quit) break;

        int ct    = gs->current_turn;
        int local = ct - gs->num_players;
        if (local >= 0 && local < g_num_enemies && gs->entities[ct].alive)
            sem_post(&npc_sem[local]);
    }
    return nullptr;
}

/* ── Main ────────────────────────────────────────────────────────── */
int main(int argc, char* argv[])
{
    if (argc < 4) {
        fprintf(stderr, "Usage: asp <roll_no> <num_players> <num_enemies>\n");
        return 1;
    }
    g_num_players = atoi(argv[2]);
    g_num_enemies = atoi(argv[3]);

    gs = shm_open_existing();

    struct sigaction sa{};
    sa.sa_handler = sig_term;  sigaction(SIGTERM, &sa, nullptr);
    sa.sa_handler = sig_stun;  sigaction(SIGUSR1, &sa, nullptr);
    /* SIGSTOP and SIGCONT: OS handles these — no override */

    for (int i = 0; i < g_num_enemies; ++i)
        sem_init(&npc_sem[i], 0, 0);

    pthread_t disp_tid;
    pthread_create(&disp_tid, nullptr, dispatcher, nullptr);

    pthread_t npc_tids[MAX_ENEMIES];
    for (int i = 0; i < g_num_enemies; ++i) {
        auto* a = new NpcArg{i, g_num_players + i};
        pthread_create(&npc_tids[i], nullptr, npc_thread, a);
    }

    while (!g_quit && gs->phase != GamePhase::GAME_OVER)
        sleep(1);

    g_quit = 1;
    for (int i = 0; i < g_num_enemies; ++i) sem_post(&npc_sem[i]);
    sem_post(&gs->turn_sem);

    pthread_join(disp_tid, nullptr);
    for (int i = 0; i < g_num_enemies; ++i)
        pthread_join(npc_tids[i], nullptr);
    for (int i = 0; i < g_num_enemies; ++i)
        sem_destroy(&npc_sem[i]);

    munmap(gs, sizeof(SharedState));
    return 0;
}
