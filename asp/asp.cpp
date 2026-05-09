
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

static SharedState *gs = nullptr;
static int g_num_enemies = 0;
static int g_num_players = 0;
static volatile sig_atomic_t g_quit = 0;
static volatile sig_atomic_t g_stun_flag = 0;

static sem_t npc_sem[MAX_ENEMIES];

static void sig_term(int)
{
    g_quit = 1;
}

static void sig_stun(int)
{
    g_stun_flag = 1;
    int ct = gs->current_turn;
    if (ct >= gs->num_players && ct < gs->total_entities)
        gs->entities[ct].stunned = true;

    sleep(STUN_DURATION);

    if (ct >= gs->num_players && ct < gs->total_entities)
    {
        gs->entities[ct].stunned = false;
        gs->entities[ct].stamina = 0;
    }
    g_stun_flag = 0;
}

static SharedState *shm_open_existing()
{
    int fd = -1;
    for (int i = 0; i < 30; ++i)
    {
        fd = shm_open(SHM_NAME, O_RDWR, 0666);
        if (fd >= 0)
            break;
        usleep(100000);
    }
    if (fd < 0)
    {
        perror("asp: shm_open");
        exit(1);
    }
    void *p = mmap(nullptr, sizeof(SharedState),
                   PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED)
    {
        perror("asp: mmap");
        exit(1);
    }
    close(fd);
    return static_cast<SharedState *>(p);
}

static Action npc_decide(int entity_idx)
{
    Action act{};

    int best = -1, best_hp = INT_MAX;
    for (int i = 0; i < gs->num_players; ++i)
    {
        const Entity &p = gs->entities[i];
        if (p.alive && p.hp < best_hp)
        {
            best = i;
            best_hp = p.hp;
        }
    }
    if (best < 0)
    {
        act.type = ActionType::SKIP;
        return act;
    }

    unsigned r = static_cast<unsigned>(entity_idx) * 1664525u + 1013904223u;
    r ^= static_cast<unsigned>(gs->entities[entity_idx].stamina * 100.0f);

    if ((r & 0xF) < 13)
    {
        act.type = ActionType::STRIKE;
        act.target_idx = best;
    }
    else
    {
        act.type = ActionType::SKIP;
    }
    return act;
}

struct NpcArg
{
    int local_idx;
    int entity_idx;
};

static void *npc_thread(void *arg_)
{
    NpcArg *arg = static_cast<NpcArg *>(arg_);
    int local = arg->local_idx;
    int eidx = arg->entity_idx;
    delete arg;

    gs->entities[eidx].pid = getpid();

    while (!g_quit)
    {
        sem_wait(&npc_sem[local]);
        if (g_quit)
            break;

        bool our_turn = (gs->current_turn == eidx &&
                         gs->turn_ready &&
                         gs->entities[eidx].alive);
        if (!our_turn)
            continue;

        if (gs->entities[eidx].stunned || g_stun_flag)
        {
            Action skip{ActionType::SKIP, -1, -1, WPN_NONE};
            sem_wait(&gs->state_mutex);
            gs->entities[eidx].pending_action = skip;
            gs->entities[eidx].action_ready = true;
            sem_post(&gs->state_mutex);
            sem_post(&gs->action_sem);
            continue;
        }

        Action act = npc_decide(eidx);

        sem_wait(&gs->state_mutex);
        gs->entities[eidx].pending_action = act;
        gs->entities[eidx].action_ready = true;
        sem_post(&gs->state_mutex);

        sem_post(&gs->action_sem);

        while (!g_quit && !gs->entities[eidx].action_done)
            usleep(5000);
    }
    return nullptr;
}

static void *dispatcher(void *)
{
    while (!g_quit)
    {
        sem_wait(&gs->asp_turn_sem);
        if (g_quit)
            break;

        int ct = gs->current_turn;
        int local = ct - gs->num_players;
        if (local >= 0 && local < g_num_enemies && gs->entities[ct].alive)
            sem_post(&npc_sem[local]);
    }
    return nullptr;
}

int main(int argc, char *argv[])
{
    if (argc < 4)
    {
        fprintf(stderr, "Usage: asp <roll_no> <num_players> <num_enemies>\n");
        return 1;
    }
    g_num_players = atoi(argv[2]);
    g_num_enemies = atoi(argv[3]);

    gs = shm_open_existing();

    struct sigaction sa{};
    sa.sa_handler = sig_term;
    sigaction(SIGTERM, &sa, nullptr);
    sa.sa_handler = sig_stun;
    sigaction(SIGUSR1, &sa, nullptr);

    for (int i = 0; i < g_num_enemies; ++i)
        sem_init(&npc_sem[i], 0, 0);

    pthread_t disp_tid;
    pthread_create(&disp_tid, nullptr, dispatcher, nullptr);

    pthread_t npc_tids[MAX_ENEMIES];
    for (int i = 0; i < g_num_enemies; ++i)
    {
        auto *a = new NpcArg{i, g_num_players + i};
        pthread_create(&npc_tids[i], nullptr, npc_thread, a);
    }

    while (!g_quit && gs->phase != GamePhase::GAME_OVER)
        sleep(1);

    g_quit = 1;
    for (int i = 0; i < g_num_enemies; ++i)
        sem_post(&npc_sem[i]);
    sem_post(&gs->asp_turn_sem);

    pthread_join(disp_tid, nullptr);
    for (int i = 0; i < g_num_enemies; ++i)
        pthread_join(npc_tids[i], nullptr);
    for (int i = 0; i < g_num_enemies; ++i)
        sem_destroy(&npc_sem[i]);

    munmap(gs, sizeof(SharedState));
    return 0;
}
