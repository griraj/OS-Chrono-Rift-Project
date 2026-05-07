/*
 * arbiter.cpp  –  Game Arbiter Process (produces 'arbiter_bin')
 *
 * Responsibilities:
 *  - Creates and owns the POSIX shared memory segment
 *  - Initialises all entities with roll-number-seeded stats
 *  - Runs the stamina-based scheduler (1 s ticks)
 *  - Enforces serial action execution
 *  - Applies all actions and updates game state
 *  - Spawns hip and asp child processes
 *  - Spawns the SFML rendering thread (replaces ncurses)
 *  - Delivers SIGUSR1 (stun) to target process
 *  - Sends SIGSTOP/SIGCONT to asp for Ultimate pause (10 s via SIGALRM)
 *  - Handles SIGTERM (quit from hip)
 *  - Runs background deadlock-monitor thread
 *  - Manages process lifecycle (wait/reap children on game over)
 *
 * SFML NOTE:
 *  SFML windows MUST be created and used exclusively from the thread that
 *  owns them. The rendering thread therefore creates the sf::RenderWindow
 *  itself. The main/game threads never touch it.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <pthread.h>
#include <time.h>
#include <semaphore.h>
#include <errno.h>
#include <climits>
#include <string>
#include <sstream>

/* SFML headers */
#include <SFML/Graphics.hpp>
#include <SFML/System.hpp>
#include <SFML/Window.hpp>

#include "shared_state.h"
#include "inventory.h"

/* ── Forward declarations ─────────────────────────────────────────── */
static void* render_thread_fn(void*);
static void  game_log(const char* msg);

/* ── Globals ──────────────────────────────────────────────────────── */
static SharedState*              gs          = nullptr;
static pid_t                     g_asp_pid   = 0;
static pid_t                     g_hip_pid   = 0;
static volatile sig_atomic_t     g_quit      = 0;
static volatile sig_atomic_t     g_ultimate  = 0;
static unsigned                  g_seed      = 0;
static volatile sig_atomic_t     g_render_done = 0;

/* ── Signal handlers ──────────────────────────────────────────────── */
static void sig_term(int) { g_quit = 1; }

/* SIGALRM: Ultimate window (10 s) expired — resume asp */
static void sig_alrm(int)
{
    g_ultimate = 0;
    if (g_asp_pid > 0) kill(g_asp_pid, SIGCONT);
    sem_post(&gs->turn_sem);
}

/* ── Shared-memory helpers ────────────────────────────────────────── */
static SharedState* shm_create()
{
    shm_unlink(SHM_NAME);
    int fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (fd < 0) { perror("shm_open"); exit(1); }
    if (ftruncate(fd, sizeof(SharedState)) < 0) { perror("ftruncate"); exit(1); }
    void* p = mmap(nullptr, sizeof(SharedState),
                   PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { perror("mmap"); exit(1); }
    close(fd);
    return static_cast<SharedState*>(p);
}

static void shm_destroy()
{
    if (gs) munmap(gs, sizeof(SharedState));
    shm_unlink(SHM_NAME);
}

/* ── Entity initialisation ────────────────────────────────────────── */
static void init_player(Entity& e, int idx, int num_players,
                         unsigned roll_no, unsigned& seed, const char* name)
{
    e = Entity{};
    e.idx         = idx;
    e.type        = EntityType::PLAYER;
    snprintf(e.name, sizeof(e.name), "%s", name);
    e.max_hp      = static_cast<int>(roll_no) + rng_range(seed, 100, 1000);
    e.hp          = e.max_hp;
    e.damage      = static_cast<int>(roll_no % 10) + 10;
    e.speed       = 100 / num_players;
    e.max_stamina = 100;
    e.stamina     = 0.0f;
    e.alive       = true;
}

static void init_enemy(Entity& e, int idx, unsigned roll_no,
                        unsigned& seed, int local_idx)
{
    e = Entity{};
    e.idx         = idx;
    e.type        = EntityType::ENEMY;
    snprintf(e.name, sizeof(e.name), "Enemy-%d", local_idx + 1);
    e.max_hp      = static_cast<int>(roll_no % 100) + rng_range(seed, 50, 200);
    e.hp          = e.max_hp;
    e.damage      = static_cast<int>((roll_no / 10) % 10) + 10;
    e.speed       = rng_range(seed, 10, 30);
    e.max_stamina = 150;
    e.stamina     = 0.0f;
    e.alive       = true;
}

/* ── Shared state initialisation ──────────────────────────────────── */
static void init_shared_state(int num_players, int num_enemies, unsigned roll_no)
{
    *gs = SharedState{};

    /* pshared=1 → semaphores shared across processes */
    sem_init(&gs->state_mutex,    1, 1);
    sem_init(&gs->action_sem,     1, 0);
    sem_init(&gs->turn_sem,       1, 0);
    sem_init(&gs->artifact_mutex, 1, 1);
    sem_init(&gs->log_mutex,      1, 1);

    gs->num_players    = num_players;
    gs->num_enemies    = num_enemies;
    gs->total_entities = num_players + num_enemies;
    gs->phase          = GamePhase::RUNNING;
    gs->seed           = roll_no;
    gs->arbiter_pid    = getpid();
    g_seed             = roll_no;

    char pname[32];
    for (int i = 0; i < num_players; ++i) {
        snprintf(pname, sizeof(pname), "Player-%d", i + 1);
        init_player(gs->entities[i], i, num_players, roll_no, g_seed, pname);
    }
    for (int i = 0; i < num_enemies; ++i)
        init_enemy(gs->entities[num_players + i], num_players + i,
                   roll_no, g_seed, i);

    /* Artifact table */
    gs->artifacts[0] = {WPN_SOLAR_CORE,    ArtifactState::FREE, -1, true };
    gs->artifacts[1] = {WPN_LUNAR_BLADE,   ArtifactState::FREE, -1, true };
    gs->artifacts[2] = {WPN_ECLIPSE_RELIC, ArtifactState::FREE, -1, false};
}

/* ── Logging ──────────────────────────────────────────────────────── */
static void game_log(const char* msg)
{
    sem_wait(&gs->log_mutex);
    int h = gs->log_head;
    strncpy(gs->log[h], msg, LOG_LINE_LEN - 1);
    gs->log[h][LOG_LINE_LEN - 1] = '\0';
    gs->log_head = (h + 1) % LOG_LINES;
    sem_post(&gs->log_mutex);
}

/* ── Apply action ─────────────────────────────────────────────────── */
static void apply_action(int actor_idx, const Action& act)
{
    sem_wait(&gs->state_mutex);

    Entity& actor = gs->entities[actor_idx];
    char buf[256];

    switch (act.type) {

    case ActionType::STRIKE: {
        Entity& tgt = gs->entities[act.target_idx];
        tgt.hp -= actor.damage;
        snprintf(buf, sizeof(buf), "%s strikes %s for %d dmg",
                 actor.name, tgt.name, actor.damage);
        actor.stamina = 0;
        if (tgt.hp <= 0) {
            tgt.hp = 0; tgt.alive = false;
            if (tgt.type == EntityType::ENEMY) ++gs->enemies_killed;
        }
        break;
    }
    case ActionType::EXHAUST: {
        Entity& tgt = gs->entities[act.target_idx];
        tgt.stamina -= static_cast<float>(actor.damage);
        if (tgt.stamina < 0) tgt.stamina = 0;
        snprintf(buf, sizeof(buf), "%s exhausts %s for %d stamina",
                 actor.name, tgt.name, actor.damage);
        actor.stamina = 0;
        break;
    }
    case ActionType::USE_WEAPON: {
        Entity& tgt = gs->entities[act.target_idx];
        WeaponID wid = actor.inventory.slots[act.weapon_slot];
        int dmg = weapon_def(wid).damage;
        tgt.hp -= dmg;
        snprintf(buf, sizeof(buf), "%s uses %s on %s for %d dmg",
                 actor.name, weapon_def(wid).name, tgt.name, dmg);
        actor.stamina = 0;
        if (tgt.hp <= 0) {
            tgt.hp = 0; tgt.alive = false;
            if (tgt.type == EntityType::ENEMY) ++gs->enemies_killed;
        }
        break;
    }
    case ActionType::ULTIMATE: {
        snprintf(buf, sizeof(buf), "*** %s triggers ULTIMATE ABILITY! ***", actor.name);
        actor.stamina = 0;
        for (int i = gs->num_players; i < gs->total_entities; ++i) {
            Entity& tgt = gs->entities[i];
            if (!tgt.alive) continue;
            tgt.hp -= weapon_def(WPN_SOLAR_CORE).damage + weapon_def(WPN_LUNAR_BLADE).damage;
            if (tgt.hp <= 0) {
                tgt.hp = 0; tgt.alive = false; ++gs->enemies_killed;
            }
        }
        break;
    }
    case ActionType::SWAP_IN: {
        inv_swap_in(actor, act.swap_wpn);
        snprintf(buf, sizeof(buf), "%s swaps in %s from LTS (costs turn)",
                 actor.name, weapon_def(act.swap_wpn).name);
        actor.stamina = 0;
        break;
    }
    case ActionType::HEAL: {
        int healed = actor.max_hp / 10;
        actor.hp += healed;
        if (actor.hp > actor.max_hp) actor.hp = actor.max_hp;
        snprintf(buf, sizeof(buf), "%s heals for %d HP", actor.name, healed);
        actor.stamina = 0;
        break;
    }
    case ActionType::SKIP:
    default:
        snprintf(buf, sizeof(buf), "%s skips", actor.name);
        actor.stamina = static_cast<float>(actor.max_stamina) * 0.5f;
        break;
    }

    buf[sizeof(actor.last_log) - 1] = '\0';
    memcpy(actor.last_log, buf, sizeof(actor.last_log));
    sem_post(&gs->state_mutex);
    game_log(buf);
}

/* ── Stun delivery ────────────────────────────────────────────────── */
static void deliver_stun(int target_idx)
{
    Entity& tgt = gs->entities[target_idx];
    tgt.stunned = true;
    pid_t pid = tgt.pid;
    if (pid > 0) kill(pid, SIGUSR1);
    char buf[128];
    snprintf(buf, sizeof(buf), "%s is STUNNED for %d seconds!", tgt.name, STUN_DURATION);
    game_log(buf);
}

/* ── Ultimate trigger ─────────────────────────────────────────────── */
static void trigger_ultimate(int actor_idx)
{
    (void)actor_idx;
    g_ultimate = 1;
    gs->phase  = GamePhase::ULTIMATE_PAUSE;
    game_log("ULTIMATE! Strategic Process suspended for 10 seconds!");
    if (g_asp_pid > 0) kill(g_asp_pid, SIGSTOP);
    alarm(ULTIMATE_DURATION);
}

/* ── Deadlock monitor thread ──────────────────────────────────────── */
static void* deadlock_monitor(void*)
{
    while (!g_quit) {
        sleep(1);
        sem_wait(&gs->artifact_mutex);

        int holder_solar = -1, holder_lunar = -1;
        for (int a = 0; a < NUM_ARTIFACTS; ++a) {
            const ArtifactEntry& ae = gs->artifacts[a];
            if (!ae.exists || ae.state != ArtifactState::HELD) continue;
            if (ae.weapon == WPN_SOLAR_CORE)  holder_solar = ae.held_by;
            if (ae.weapon == WPN_LUNAR_BLADE) holder_lunar = ae.held_by;
        }

        if (holder_solar >= 0 && holder_lunar >= 0 && holder_solar != holder_lunar) {
            int victim = holder_solar;
            Entity& e = gs->entities[victim];

            for (int s = 0; s < INVENTORY_SLOTS; ++s) {
                if (e.inventory.slots[s] == WPN_SOLAR_CORE) {
                    inv_remove_weapon(e.inventory, s);
                    break;
                }
            }
            for (int a = 0; a < NUM_ARTIFACTS; ++a) {
                if (gs->artifacts[a].weapon == WPN_SOLAR_CORE) {
                    gs->artifacts[a].state   = ArtifactState::FREE;
                    gs->artifacts[a].held_by = -1;
                }
            }
            sem_post(&gs->artifact_mutex);

            char buf[128];
            snprintf(buf, sizeof(buf),
                     "DEADLOCK DETECTED: %s forced to release Solar Core", e.name);
            game_log(buf);
            continue;
        }
        sem_post(&gs->artifact_mutex);
    }
    return nullptr;
}

/* ── Win / Lose / Quit check ──────────────────────────────────────── */
static bool check_end_conditions()
{
    if (g_quit) {
        gs->result = GameResult::QUIT;
        gs->phase  = GamePhase::GAME_OVER;
        return true;
    }
    if (gs->enemies_killed >= MAX_ENEMIES_KILL) {
        gs->result = GameResult::WIN;
        gs->phase  = GamePhase::GAME_OVER;
        game_log("VICTORY! 10 enemies defeated!");
        return true;
    }
    bool any_alive = false;
    for (int i = 0; i < gs->num_players; ++i)
        if (gs->entities[i].alive) { any_alive = true; break; }
    if (!any_alive) {
        gs->result = GameResult::LOSE;
        gs->phase  = GamePhase::GAME_OVER;
        game_log("DEFEAT! All players have fallen...");
        return true;
    }
    return false;
}

/* ── Stamina scheduler tick ───────────────────────────────────────── */
static int scheduler_tick()
{
    sem_wait(&gs->state_mutex);
    int ready = -1;

    for (int i = 0; i < gs->total_entities; ++i) {
        Entity& e = gs->entities[i];
        if (!e.alive || e.stunned) continue;
        if (g_ultimate && e.type == EntityType::ENEMY) continue;

        e.stamina += static_cast<float>(e.speed);
        if (e.stamina > static_cast<float>(e.max_stamina))
            e.stamina = static_cast<float>(e.max_stamina);

        if (e.stamina >= static_cast<float>(e.max_stamina))
            if (ready < 0) ready = i;
    }
    sem_post(&gs->state_mutex);
    return ready;
}

/* ── Main game loop ───────────────────────────────────────────────── */
static void arbiter_main_loop()
{
    struct timespec tick{ 1, 0 };

    while (!check_end_conditions()) {
        int ready = scheduler_tick();
        if (ready < 0) { nanosleep(&tick, nullptr); continue; }

        Entity& actor = gs->entities[ready];

        {
            char buf[128];
            snprintf(buf, sizeof(buf), ">>> %s's turn (stamina full)", actor.name);
            game_log(buf);
        }

        sem_wait(&gs->state_mutex);
        gs->current_turn       = ready;
        gs->turn_ready         = true;
        gs->npc_submitted      = false;
        actor.action_ready     = false;
        actor.action_done      = false;
        sem_post(&gs->state_mutex);

        sem_post(&gs->turn_sem);

        if (actor.type == EntityType::PLAYER) {
            sem_wait(&gs->action_sem);
        } else {
            struct timespec deadline{};
            clock_gettime(CLOCK_REALTIME, &deadline);
            deadline.tv_sec += NPC_TURN_TIMEOUT;
            int rc = sem_timedwait(&gs->action_sem, &deadline);
            if (rc != 0) {
                sem_wait(&gs->state_mutex);
                actor.pending_action = {ActionType::SKIP, -1, -1, WPN_NONE};
                actor.action_ready   = true;
                sem_post(&gs->state_mutex);
                game_log("NPC turn timed out — auto Skip");
            }
        }

        sem_wait(&gs->state_mutex);
        Action act   = actor.pending_action;
        actor.action_done = true;
        sem_post(&gs->state_mutex);

        bool ultimate_triggered = (act.type == ActionType::ULTIMATE);
        bool stun_triggered = false;
        if ((act.type == ActionType::STRIKE || act.type == ActionType::USE_WEAPON)
            && act.target_idx >= 0 && gs->entities[act.target_idx].alive) {
            unsigned tmp = g_seed;
            stun_triggered = (rng_range(tmp, 1, 5) == 1);
            g_seed = tmp;
        }

        apply_action(ready, act);

        if (ultimate_triggered) trigger_ultimate(ready);
        if (stun_triggered)     deliver_stun(act.target_idx);

        for (int i = gs->num_players; i < gs->total_entities; ++i) {
            Entity& en = gs->entities[i];
            if (!en.alive && en.stamina >= 0.0f) {
                char buf[128];
                snprintf(buf, sizeof(buf), "%s has been defeated!", en.name);
                game_log(buf);
                en.stamina = -1.0f;
            }
        }

        if (check_end_conditions()) break;
        nanosleep(&tick, nullptr);
    }
}

/* ══════════════════════════════════════════════════════════════════
 * SFML Rendering Thread
 *
 * Design:
 *  - Runs entirely in its own pthread; game logic never touches SFML.
 *  - Creates the sf::RenderWindow here (SFML requires window + OpenGL
 *    context to live on the same thread).
 *  - Reads a snapshot of SharedState under the state_mutex every ~100 ms.
 *  - Renders health bars, stamina bars, artifact table, and action log.
 *  - Window close event sets g_quit so the rest of the game exits cleanly.
 * ══════════════════════════════════════════════════════════════════ */

/* Layout constants */
static const unsigned WIN_W  = 1100;
static const unsigned WIN_H  = 780;
static const float    MARGIN = 18.f;
static const float    ROW_H  = 28.f;
static const float    BAR_W  = 200.f;
static const float    BAR_H  = 14.f;

/* Colour palette */
static const sf::Color C_BG        { 15,  15,  25};
static const sf::Color C_PANEL     { 28,  28,  48};
static const sf::Color C_BORDER    { 60,  60, 100};
static const sf::Color C_TITLE     {220, 200, 100};
static const sf::Color C_WHITE     {230, 230, 230};
static const sf::Color C_ACTIVE    {255, 230,  80};
static const sf::Color C_DEAD      {100,  60,  60};
static const sf::Color C_STUN      {180, 100, 255};
static const sf::Color C_HP_PLAYER { 60, 200,  80};
static const sf::Color C_HP_ENEMY  {220,  60,  60};
static const sf::Color C_STAMINA   { 60, 180, 255};
static const sf::Color C_LOG_BG    { 20,  20,  35};
static const sf::Color C_LOG_TEXT  {180, 200, 160};
static const sf::Color C_ARTIFACT  {255, 200,  80};
static const sf::Color C_ULTIMATE  {255, 100, 200};
static const sf::Color C_SECTION   { 80,  80, 140};

/* ── Helper: draw a filled bar ──────────────────────────────────────── */
static void draw_bar_sfml(sf::RenderTarget& rt,
                           float x, float y, float w, float h,
                           float val, float max_val,
                           sf::Color filled_col)
{
    /* background */
    sf::RectangleShape bg({w, h});
    bg.setPosition(x, y);
    bg.setFillColor({40, 40, 60});
    bg.setOutlineColor(C_BORDER);
    bg.setOutlineThickness(1.f);
    rt.draw(bg);

    if (max_val > 0.f) {
        float ratio = val / max_val;
        if (ratio < 0.f) ratio = 0.f;
        if (ratio > 1.f) ratio = 1.f;
        sf::RectangleShape fill({w * ratio, h});
        fill.setPosition(x, y);
        fill.setFillColor(filled_col);
        rt.draw(fill);
    }
}

/* ── Helper: draw a rounded panel ───────────────────────────────────── */
static void draw_panel(sf::RenderTarget& rt, float x, float y, float w, float h)
{
    sf::RectangleShape panel({w, h});
    panel.setPosition(x, y);
    panel.setFillColor(C_PANEL);
    panel.setOutlineColor(C_BORDER);
    panel.setOutlineThickness(1.5f);
    rt.draw(panel);
}

/* ── Helper: section header line ────────────────────────────────────── */
static void draw_section_line(sf::RenderTarget& rt,
                               float x, float y, float w,
                               const std::string& label,
                               const sf::Font& font)
{
    sf::RectangleShape line({w, 1.5f});
    line.setPosition(x, y + 10.f);
    line.setFillColor(C_SECTION);
    rt.draw(line);

    sf::Text t(label, font, 13);
    t.setFillColor(C_SECTION);
    t.setPosition(x + 4.f, y - 1.f);
    rt.draw(t);
}

/* ── Render one entity row ───────────────────────────────────────────── */
static void render_entity_row_sfml(sf::RenderTarget& rt,
                                    float x, float y, float panel_w,
                                    const Entity& e, bool active,
                                    const sf::Font& font)
{
    /* Row background highlight for active entity */
    if (active) {
        sf::RectangleShape hl({panel_w - 4.f, ROW_H - 2.f});
        hl.setPosition(x + 2.f, y);
        hl.setFillColor({40, 40, 10, 80});
        rt.draw(hl);
    }

    /* Name */
    sf::Color name_col = active    ? C_ACTIVE
                       : !e.alive  ? C_DEAD
                       : e.stunned ? C_STUN
                       :             C_WHITE;

    sf::Text name_t(e.name, font, 13);
    name_t.setFillColor(name_col);
    name_t.setPosition(x + 6.f, y + 5.f);
    if (active) name_t.setStyle(sf::Text::Bold);
    rt.draw(name_t);

    /* Status badge */
    const char* badge = !e.alive ? "[DEAD]" : e.stunned ? "[STUN]" : nullptr;
    if (badge) {
        sf::Text bt(badge, font, 11);
        bt.setFillColor(e.stunned ? C_STUN : C_DEAD);
        bt.setPosition(x + 115.f, y + 7.f);
        rt.draw(bt);
    }

    /* HP bar */
    float bx = x + 165.f;
    draw_bar_sfml(rt, bx, y + 7.f, BAR_W, BAR_H,
                  static_cast<float>(e.hp), static_cast<float>(e.max_hp),
                  e.type == EntityType::PLAYER ? C_HP_PLAYER : C_HP_ENEMY);

    std::ostringstream hpss;
    hpss << e.hp << "/" << e.max_hp;
    sf::Text hp_t(hpss.str(), font, 11);
    hp_t.setFillColor(C_WHITE);
    hp_t.setPosition(bx + BAR_W + 4.f, y + 6.f);
    rt.draw(hp_t);

    /* Stamina bar */
    float sx = bx + BAR_W + 70.f;
    float st_val = e.stamina < 0.f ? 0.f : e.stamina;
    draw_bar_sfml(rt, sx, y + 7.f, 120.f, BAR_H,
                  st_val, static_cast<float>(e.max_stamina),
                  C_STAMINA);

    std::ostringstream stss;
    stss << static_cast<int>(st_val) << "/" << e.max_stamina;
    sf::Text st_t(stss.str(), font, 11);
    st_t.setFillColor(C_WHITE);
    st_t.setPosition(sx + 124.f, y + 6.f);
    rt.draw(st_t);
}

/* ── SFML render thread function ─────────────────────────────────────── */
static void* render_thread_fn(void*)
{
    /* ── Window creation (must happen in this thread) ── */
    sf::RenderWindow window(
        sf::VideoMode(WIN_W, WIN_H),
        "Chrono Rift  |  CS 2006 Operating Systems",
        sf::Style::Titlebar | sf::Style::Close
    );
    window.setFramerateLimit(10); /* 10 fps — non-blocking, low CPU */

    /* ── Font ── */
    sf::Font font;
    /* Try several common monospace font paths on Ubuntu */
    const char* font_paths[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/truetype/ubuntu/UbuntuMono-R.ttf",
        "/usr/share/fonts/truetype/freefont/FreeMono.ttf",
        nullptr
    };
    bool font_loaded = false;
    for (int i = 0; font_paths[i]; ++i) {
        if (font.loadFromFile(font_paths[i])) {
            font_loaded = true;
            break;
        }
    }
    if (!font_loaded) {
        fprintf(stderr,
            "[Renderer] WARNING: Could not load a monospace font. "
            "Install fonts-dejavu-core or fonts-liberation.\n");
        /* SFML will use its built-in bitmap font as fallback — still works */
    }

    /* ── Snapshot buffer ── */
    SharedState snap{};

    while (window.isOpen() && !g_quit && !g_render_done) {

        /* ── Process SFML events ── */
        sf::Event ev{};
        while (window.pollEvent(ev)) {
            if (ev.type == sf::Event::Closed) {
                /* User closed the window — signal a quit */
                g_quit = 1;
                window.close();
            }
        }

        /* ── Snapshot shared state (brief lock) ── */
        sem_wait(&gs->state_mutex);
        memcpy(&snap, gs, sizeof(snap));
        sem_post(&gs->state_mutex);

        /* ── Clear ── */
        window.clear(C_BG);

        float cx = MARGIN;       /* current X cursor */
        float cy = MARGIN;       /* current Y cursor */

        /* ════════════════════ TITLE BAR ════════════════════ */
        {
            const char* phase_str =
                snap.phase == GamePhase::ULTIMATE_PAUSE ? "  *** ULTIMATE PAUSE ***" :
                snap.phase == GamePhase::GAME_OVER      ? "  GAME OVER"              :
                                                          "  RUNNING";
            sf::Color phase_col =
                snap.phase == GamePhase::ULTIMATE_PAUSE ? C_ULTIMATE :
                snap.phase == GamePhase::GAME_OVER      ? C_HP_ENEMY :
                                                          C_HP_PLAYER;

            std::ostringstream title;
            title << "CHRONO RIFT      Kills: "
                  << snap.enemies_killed << " / " << MAX_ENEMIES_KILL
                  << phase_str;

            sf::Text t(title.str(), font, 16);
            t.setFillColor(phase_col);
            t.setStyle(sf::Text::Bold);
            t.setPosition(cx, cy);
            window.draw(t);
        }
        cy += 30.f;

        /* ════════════════════ COLUMN LAYOUT ════════════════════
         * Left column : Players + Enemies
         * Right column: Artifacts + Action Log
         * ════════════════════════════════════════════════════════ */
        float left_w  = WIN_W * 0.58f;
        float right_x = cx + left_w + 12.f;
        float right_w = WIN_W - right_x - MARGIN;
        float top_y   = cy;

        /* ── PLAYERS PANEL ── */
        float players_h = MARGIN + snap.num_players * ROW_H + MARGIN;
        draw_panel(window, cx, cy, left_w, players_h);
        draw_section_line(window, cx + 8.f, cy + 6.f, left_w - 16.f, "PLAYERS", font);

        /* Column headers */
        {
            sf::Text hdr("Name           Status  HP Bar                             HP         Stamina Bar      ST", font, 11);
            hdr.setFillColor(C_SECTION);
            hdr.setPosition(cx + 6.f, cy + 18.f);
            window.draw(hdr);
        }

        float ey = cy + MARGIN + 12.f;
        for (int i = 0; i < snap.num_players; ++i, ey += ROW_H)
            render_entity_row_sfml(window, cx, ey, left_w,
                                   snap.entities[i],
                                   snap.current_turn == i, font);
        cy += players_h + 10.f;

        /* ── ENEMIES PANEL ── */
        float enemies_h = MARGIN + snap.num_enemies * ROW_H + MARGIN;
        draw_panel(window, cx, cy, left_w, enemies_h);
        draw_section_line(window, cx + 8.f, cy + 6.f, left_w - 16.f, "ENEMIES", font);

        ey = cy + MARGIN + 12.f;
        for (int i = 0; i < snap.num_enemies; ++i, ey += ROW_H) {
            int eidx = snap.num_players + i;
            render_entity_row_sfml(window, cx, ey, left_w,
                                   snap.entities[eidx],
                                   snap.current_turn == eidx, font);
        }
        cy += enemies_h + 10.f;

        /* ── RIGHT COLUMN: ARTIFACTS ── */
        float ry = top_y;
        int   artifact_count = 0;
        for (int a = 0; a < NUM_ARTIFACTS; ++a)
            if (snap.artifacts[a].exists) ++artifact_count;

        float art_h = MARGIN + artifact_count * ROW_H + MARGIN;
        draw_panel(window, right_x, ry, right_w, art_h);
        draw_section_line(window, right_x + 8.f, ry + 6.f, right_w - 16.f, "ARTIFACTS", font);

        float ay = ry + MARGIN + 12.f;
        for (int a = 0; a < NUM_ARTIFACTS; ++a) {
            const ArtifactEntry& ae = snap.artifacts[a];
            if (!ae.exists) continue;

            const char* holder_name =
                (ae.state == ArtifactState::HELD && ae.held_by >= 0)
                    ? snap.entities[ae.held_by].name
                    : "FREE";
            sf::Color ac = (ae.state == ArtifactState::HELD) ? C_ARTIFACT : C_SECTION;

            std::ostringstream aoss;
            aoss << weapon_def(ae.weapon).name << " : " << holder_name;
            sf::Text at(aoss.str(), font, 13);
            at.setFillColor(ac);
            at.setPosition(right_x + 8.f, ay + 4.f);
            window.draw(at);
            ay += ROW_H;
        }
        ry += art_h + 10.f;

        /* ── RIGHT COLUMN: ACTION LOG ── */
        float log_h = WIN_H - ry - MARGIN - 10.f;
        if (log_h < 60.f) log_h = 60.f;
        draw_panel(window, right_x, ry, right_w, log_h);
        draw_section_line(window, right_x + 8.f, ry + 6.f, right_w - 16.f, "ACTION LOG", font);

        /* Display last 14 log lines, oldest first */
        int   max_lines = static_cast<int>((log_h - MARGIN - 12.f) / 18.f);
        if (max_lines > LOG_LINES) max_lines = LOG_LINES;
        float ly = ry + MARGIN + 12.f;
        for (int l = 0; l < max_lines; ++l) {
            int idx = ((snap.log_head - max_lines + l + LOG_LINES) % LOG_LINES);
            if (snap.log[idx][0] == '\0') continue;

            /* Colour-code: ULTIMATE / STUN / DEADLOCK / normal */
            sf::Color lc = C_LOG_TEXT;
            std::string line_str = snap.log[idx];
            if (line_str.find("ULTIMATE") != std::string::npos)  lc = C_ULTIMATE;
            else if (line_str.find("STUN")     != std::string::npos)  lc = C_STUN;
            else if (line_str.find("DEADLOCK") != std::string::npos)  lc = C_HP_ENEMY;
            else if (line_str.find("DEFEAT")   != std::string::npos)  lc = C_HP_ENEMY;
            else if (line_str.find("VICTORY")  != std::string::npos)  lc = C_ACTIVE;

            /* Truncate if too wide for column */
            if (line_str.size() > 42) line_str = line_str.substr(0, 39) + "...";

            sf::Text lt(line_str, font, 12);
            lt.setFillColor(lc);
            lt.setPosition(right_x + 8.f, ly);
            window.draw(lt);
            ly += 18.f;
        }

        /* ── GAME OVER OVERLAY ── */
        if (snap.phase == GamePhase::GAME_OVER) {
            sf::RectangleShape overlay({static_cast<float>(WIN_W), static_cast<float>(WIN_H)});
            overlay.setFillColor({0, 0, 0, 160});
            window.draw(overlay);

            const char* msg =
                snap.result == GameResult::WIN  ? "VICTORY!" :
                snap.result == GameResult::LOSE ? "DEFEAT!" :
                                                  "GAME QUIT";
            sf::Color mc = (snap.result == GameResult::WIN) ? C_ACTIVE : C_HP_ENEMY;

            sf::Text mt(msg, font, 56);
            mt.setFillColor(mc);
            mt.setStyle(sf::Text::Bold);
            sf::FloatRect mb = mt.getLocalBounds();
            mt.setOrigin(mb.width / 2.f, mb.height / 2.f);
            mt.setPosition(WIN_W / 2.f, WIN_H / 2.f - 30.f);
            window.draw(mt);

            sf::Text sub("Window will close in a moment...", font, 18);
            sub.setFillColor(C_WHITE);
            sf::FloatRect sb = sub.getLocalBounds();
            sub.setOrigin(sb.width / 2.f, sb.height / 2.f);
            sub.setPosition(WIN_W / 2.f, WIN_H / 2.f + 40.f);
            window.draw(sub);
        }

        window.display();

        /* Close window automatically 3 s after game over */
        if (snap.phase == GamePhase::GAME_OVER) {
            sf::sleep(sf::seconds(3.f));
            window.close();
        }
    }

    if (window.isOpen()) window.close();
    return nullptr;
}

/* ══════════════════════════════════════════════════════════════════
 * arbiter_main  (called from main_launcher.cpp)
 * ══════════════════════════════════════════════════════════════════ */
int arbiter_main(int argc, char* argv[])
{
    if (argc < 3) {
        fprintf(stderr, "Usage: arbiter <roll_number> <num_players>\n");
        return 1;
    }

    unsigned roll_no     = static_cast<unsigned>(atoi(argv[1]));
    int      num_players = atoi(argv[2]);
    if (num_players < 1) num_players = 1;
    if (num_players > 4) num_players = 4;

    g_seed = roll_no;
    int num_enemies = rng_range(g_seed, 2, 9);

    printf("[Arbiter] seed=%u | players=%d | enemies=%d\n",
           roll_no, num_players, num_enemies);
    fflush(stdout);

    /* Create shared memory and initialise */
    gs = shm_create();
    init_shared_state(num_players, num_enemies, roll_no);

    /* Install signal handlers */
    struct sigaction sa{};
    sa.sa_handler = sig_term; sigaction(SIGTERM, &sa, nullptr);
    sa.sa_handler = sig_alrm; sigaction(SIGALRM, &sa, nullptr);

    /* ── Start SFML rendering thread ── */
    pthread_t render_tid;
    pthread_create(&render_tid, nullptr, render_thread_fn, nullptr);

    /* ── Start deadlock monitor thread ── */
    pthread_t dl_tid;
    pthread_create(&dl_tid, nullptr, deadlock_monitor, nullptr);

    /* ── Spawn HIP ── */
    char roll_str[16], np_str[8];
    snprintf(roll_str, sizeof(roll_str), "%u", roll_no);
    snprintf(np_str,   sizeof(np_str),   "%d", num_players);

    g_hip_pid = fork();
    if (g_hip_pid == 0) {
        execlp("./hip_bin", "./hip_bin", roll_str, np_str, nullptr);
        perror("exec hip"); _exit(1);
    }
    gs->hip_pid = g_hip_pid;

    /* ── Spawn ASP ── */
    char ne_str[8];
    snprintf(ne_str, sizeof(ne_str), "%d", num_enemies);
    g_asp_pid = fork();
    if (g_asp_pid == 0) {
        execlp("./asp_bin", "./asp_bin", roll_str, np_str, ne_str, nullptr);
        perror("exec asp"); _exit(1);
    }
    gs->asp_pid = g_asp_pid;

    /* ── Main game loop ── */
    arbiter_main_loop();

    /* ── Cleanup ── */
    g_quit = 1;
    g_render_done = 1;

    if (g_hip_pid > 0) kill(g_hip_pid, SIGTERM);
    if (g_asp_pid > 0) { kill(g_asp_pid, SIGCONT); kill(g_asp_pid, SIGTERM); }

    waitpid(g_hip_pid, nullptr, 0);
    waitpid(g_asp_pid, nullptr, 0);

    pthread_join(render_tid, nullptr);
    pthread_join(dl_tid, nullptr);

    sem_destroy(&gs->state_mutex);
    sem_destroy(&gs->action_sem);
    sem_destroy(&gs->turn_sem);
    sem_destroy(&gs->artifact_mutex);
    sem_destroy(&gs->log_mutex);

    switch (gs->result) {
    case GameResult::WIN:  printf("\n=== VICTORY! You defeated %d enemies! ===\n", gs->enemies_killed); break;
    case GameResult::LOSE: printf("\n=== DEFEAT! All players have fallen. ===\n"); break;
    case GameResult::QUIT: printf("\n=== Game quit by player. ===\n"); break;
    default: break;
    }

    shm_destroy();
    return 0;
}
