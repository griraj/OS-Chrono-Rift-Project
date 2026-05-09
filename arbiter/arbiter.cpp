
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
#include <cmath>
#include <string>
#include <sstream>

#include <SFML/Graphics.hpp>
#include <SFML/System.hpp>
#include <SFML/Window.hpp>

#include "shared_state.h"
#include "inventory.h"
#include "sprite_draw.inl"

static void *render_thread_fn(void *);
static void game_log(const char *msg);

static SharedState *gs = nullptr;
static pid_t g_asp_pid = 0;
static pid_t g_hip_pid = 0;
static volatile sig_atomic_t g_quit = 0;
static volatile sig_atomic_t g_ultimate = 0;
static unsigned g_seed = 0;
static volatile sig_atomic_t g_render_done = 0;

static void sig_term(int) { g_quit = 1; }

static void sig_alrm(int)
{
    g_ultimate = 0;
    if (g_asp_pid > 0) kill(g_asp_pid, SIGCONT);
    sem_post(&gs->hip_turn_sem);
    sem_post(&gs->asp_turn_sem);
}

static SharedState *shm_create()
{
    shm_unlink(SHM_NAME);
    int fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (fd < 0) { perror("shm_open"); exit(1); }
    if (ftruncate(fd, sizeof(SharedState)) < 0) { perror("ftruncate"); exit(1); }
    void *p = mmap(nullptr, sizeof(SharedState), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { perror("mmap"); exit(1); }
    close(fd);
    return static_cast<SharedState *>(p);
}

static void shm_destroy()
{
    if (gs) munmap(gs, sizeof(SharedState));
    shm_unlink(SHM_NAME);
}

static void init_player(Entity &e, int idx, int num_players,
                        unsigned roll_no, unsigned &seed, const char *name)
{
    e = Entity{};
    e.idx = idx; e.type = EntityType::PLAYER;
    snprintf(e.name, sizeof(e.name), "%s", name);
    e.max_hp = static_cast<int>(roll_no) + rng_range(seed, 100, 1000);
    e.hp = e.max_hp;
    e.damage = static_cast<int>(roll_no % 10) + 10;
    e.speed = 100 / num_players;
    e.max_stamina = 100; e.stamina = 0.0f; e.alive = true; e.swap_in_slot = -1;
}

static void init_enemy(Entity &e, int idx, unsigned roll_no, unsigned &seed, int local_idx)
{
    e = Entity{};
    e.idx = idx; e.type = EntityType::ENEMY;
    snprintf(e.name, sizeof(e.name), "Enemy-%d", local_idx + 1);
    e.max_hp = static_cast<int>(roll_no % 100) + rng_range(seed, 50, 200);
    e.hp = e.max_hp;
    e.damage = static_cast<int>((roll_no / 10) % 10) + 10;
    e.speed = rng_range(seed, 10, 30);
    e.max_stamina = 150; e.stamina = 0.0f; e.alive = true;
}

static void init_shared_state(int num_players, int num_enemies, unsigned roll_no)
{
    *gs = SharedState{};
    sem_init(&gs->state_mutex, 1, 1);
    sem_init(&gs->action_sem, 1, 0);
    sem_init(&gs->hip_turn_sem, 1, 0);
    sem_init(&gs->asp_turn_sem, 1, 0);
    gs->render_ready = false;
    sem_init(&gs->artifact_mutex, 1, 1);
    sem_init(&gs->log_mutex, 1, 1);

    gs->num_players = num_players; gs->num_enemies = num_enemies;
    gs->total_entities = num_players + num_enemies;
    gs->phase = GamePhase::RUNNING; gs->seed = roll_no;
    gs->arbiter_pid = getpid(); g_seed = roll_no;

    char pname[32];
    for (int i = 0; i < num_players; i++) {
        snprintf(pname, sizeof(pname), "Player-%d", i + 1);
        init_player(gs->entities[i], i, num_players, roll_no, g_seed, pname);
    }
    for (int i = 0; i < num_enemies; i++)
        init_enemy(gs->entities[num_players + i], num_players + i, roll_no, g_seed, i);

    gs->artifacts[0] = {WPN_SOLAR_CORE,   ArtifactState::FREE, -1, true};
    gs->artifacts[1] = {WPN_LUNAR_BLADE,  ArtifactState::FREE, -1, true};
    gs->artifacts[2] = {WPN_ECLIPSE_RELIC,ArtifactState::FREE, -1, false};

    for (int i = 0; i < MAX_ENTITIES; i++) gs->waiting_for[i] = WPN_NONE;
}

static void game_log(const char *msg)
{
    sem_wait(&gs->log_mutex);
    int h = gs->log_head;
    strncpy(gs->log[h], msg, LOG_LINE_LEN - 1);
    gs->log[h][LOG_LINE_LEN - 1] = '\0';
    gs->log_head = (h + 1) % LOG_LINES;
    sem_post(&gs->log_mutex);
}

static bool artifact_acquire(int entity_idx, WeaponID wpn)
{
    sem_wait(&gs->artifact_mutex);
    gs->waiting_for[entity_idx] = wpn;
    for (int a = 0; a < NUM_ARTIFACTS; a++) {
        ArtifactEntry& ae = gs->artifacts[a];
        if (!ae.exists || ae.weapon != wpn) continue;
        if (ae.state == ArtifactState::FREE) {
            ae.state = ArtifactState::HELD; ae.held_by = entity_idx;
            gs->waiting_for[entity_idx] = WPN_NONE;
            sem_post(&gs->artifact_mutex); return true;
        } else { sem_post(&gs->artifact_mutex); return false; }
    }
    gs->waiting_for[entity_idx] = WPN_NONE;
    sem_post(&gs->artifact_mutex); return false;
}

static void artifact_introduce_eclipse(int entity_idx)
{
    sem_wait(&gs->artifact_mutex);
    for (int a = 0; a < NUM_ARTIFACTS; a++) {
        if (gs->artifacts[a].weapon == WPN_ECLIPSE_RELIC) {
            if (!gs->artifacts[a].exists) {
                gs->artifacts[a].exists = true;
                gs->artifacts[a].state = ArtifactState::HELD;
                gs->artifacts[a].held_by = entity_idx;
            }
            break;
        }
    }
    sem_post(&gs->artifact_mutex);
}

static void artifact_release(int entity_idx, WeaponID wpn)
{
    sem_wait(&gs->artifact_mutex);
    for (int a = 0; a < NUM_ARTIFACTS; a++) {
        ArtifactEntry& ae = gs->artifacts[a];
        if (ae.weapon == wpn && ae.held_by == entity_idx) {
            ae.state = ArtifactState::FREE; ae.held_by = -1; break;
        }
    }
    gs->waiting_for[entity_idx] = WPN_NONE;
    sem_post(&gs->artifact_mutex);
}

static void artifact_on_pickup(int entity_idx, WeaponID wpn)
{
    if (!weapon_def(wpn).is_artifact) return;
    if (wpn == WPN_ECLIPSE_RELIC) {
        artifact_introduce_eclipse(entity_idx);
        game_log("Eclipse Relic introduced! It joins the global artifact pool.");
    } else {
        char buf[128];
        if (!artifact_acquire(entity_idx, wpn)) {
            snprintf(buf, sizeof(buf), "%s is waiting to acquire %s (held by another entity)",
                     gs->entities[entity_idx].name, weapon_def(wpn).name);
        } else {
            snprintf(buf, sizeof(buf), "%s acquired artifact: %s",
                     gs->entities[entity_idx].name, weapon_def(wpn).name);
        }
        game_log(buf);
    }
}

static void artifact_on_remove(int entity_idx, WeaponID wpn)
{
    if (!weapon_def(wpn).is_artifact) return;
    artifact_release(entity_idx, wpn);
    char buf[128];
    snprintf(buf, sizeof(buf), "%s released artifact: %s",
             gs->entities[entity_idx].name, weapon_def(wpn).name);
    game_log(buf);
}

static void apply_action(int actor_idx, const Action &act)
{
    sem_wait(&gs->state_mutex);
    Entity& actor = gs->entities[actor_idx];
    char buf[256];

    switch (act.type) {
    case ActionType::STRIKE: {
        Entity &tgt = gs->entities[act.target_idx];
        tgt.hp -= actor.damage;
        snprintf(buf, sizeof(buf), "%s strikes %s for %d dmg", actor.name, tgt.name, actor.damage);
        actor.stamina = 0;
        if (tgt.hp <= 0) { tgt.hp = 0; tgt.alive = false; if (tgt.type == EntityType::ENEMY) ++gs->enemies_killed; }
        break;
    }
    case ActionType::EXHAUST: {
        Entity &tgt = gs->entities[act.target_idx];
        tgt.stamina -= static_cast<float>(actor.damage);
        if (tgt.stamina < 0) tgt.stamina = 0;
        snprintf(buf, sizeof(buf), "%s exhausts %s for %d stamina", actor.name, tgt.name, actor.damage);
        actor.stamina = 0; break;
    }
    case ActionType::USE_WEAPON: {
        Entity &tgt = gs->entities[act.target_idx];
        WeaponID wid = actor.inventory.slots[act.weapon_slot];
        int dmg = weapon_def(wid).damage;
        tgt.hp -= dmg;
        snprintf(buf, sizeof(buf), "%s uses %s on %s for %d dmg", actor.name, weapon_def(wid).name, tgt.name, dmg);
        actor.stamina = 0;
        if (tgt.hp <= 0) { tgt.hp = 0; tgt.alive = false; if (tgt.type == EntityType::ENEMY) ++gs->enemies_killed; }
        break;
    }
    case ActionType::ULTIMATE: {
        snprintf(buf, sizeof(buf), "*** %s triggers ULTIMATE ABILITY! ***", actor.name);
        actor.stamina = 0;
        for (int i = gs->num_players; i < gs->total_entities; i++) {
            Entity &tgt = gs->entities[i];
            if (!tgt.alive) continue;
            tgt.hp -= weapon_def(WPN_SOLAR_CORE).damage + weapon_def(WPN_LUNAR_BLADE).damage;
            if (tgt.hp <= 0) { tgt.hp = 0; tgt.alive = false; ++gs->enemies_killed; }
        }
        break;
    }
    case ActionType::SWAP_IN: {
        inv_swap_in(actor, act.swap_wpn);
        snprintf(buf, sizeof(buf), "%s swaps in %s from LTS (costs turn)", actor.name, weapon_def(act.swap_wpn).name);
        actor.stamina = 0; break;
    }
    case ActionType::HEAL: {
        int healed = actor.max_hp / 10;
        actor.hp += healed;
        if (actor.hp > actor.max_hp) actor.hp = actor.max_hp;
        snprintf(buf, sizeof(buf), "%s heals for %d HP", actor.name, healed);
        actor.stamina = 0; break;
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

static void deliver_stun(int target_idx)
{
    Entity &tgt = gs->entities[target_idx];
    tgt.stunned = true;
    pid_t pid = tgt.pid;
    if (pid > 0) kill(pid, SIGUSR1);
    char buf[128];
    snprintf(buf, sizeof(buf), "%s is STUNNED for %d seconds!", tgt.name, STUN_DURATION);
    game_log(buf);
}

static void trigger_ultimate(int actor_idx)
{
    (void)actor_idx;
    g_ultimate = 1;
    gs->phase = GamePhase::ULTIMATE_PAUSE;
    game_log("ULTIMATE! Strategic Process suspended for 10 seconds!");
    if (g_asp_pid > 0) kill(g_asp_pid, SIGSTOP);
    alarm(ULTIMATE_DURATION);
}

static void *deadlock_monitor(void *)
{
    while (!g_quit) {
        sleep(1);
        sem_wait(&gs->artifact_mutex);

        int waits_on[MAX_ENTITIES];
        for (int i = 0; i < MAX_ENTITIES; ++i) waits_on[i] = -1;

        for (int i = 0; i < gs->total_entities; ++i) {
            WeaponID w = gs->waiting_for[i];
            if (w == WPN_NONE) continue;
            for (int a = 0; a < NUM_ARTIFACTS; ++a) {
                const ArtifactEntry &ae = gs->artifacts[a];
                if (ae.exists && ae.weapon == w && ae.state == ArtifactState::HELD) {
                    waits_on[i] = ae.held_by; break;
                }
            }
        }

        int deadlock_victim = -1; WeaponID release_wpn = WPN_NONE;
        for (int start = 0; start < gs->total_entities && deadlock_victim < 0; ++start) {
            if (waits_on[start] < 0) continue;
            bool visited[MAX_ENTITIES] = {};
            int cur = start;
            while (cur >= 0 && !visited[cur]) { visited[cur] = true; cur = waits_on[cur]; }
            if (cur >= 0) {
                deadlock_victim = cur;
                for (int a = 0; a < NUM_ARTIFACTS; ++a) {
                    const ArtifactEntry &ae = gs->artifacts[a];
                    if (ae.exists && ae.state == ArtifactState::HELD && ae.held_by == cur)
                        if (ae.weapon == WPN_SOLAR_CORE || release_wpn == WPN_NONE)
                            release_wpn = ae.weapon;
                }
            }
        }

        if (deadlock_victim >= 0 && release_wpn != WPN_NONE) {
            Entity &victim_e = gs->entities[deadlock_victim];
            sem_wait(&gs->state_mutex);
            for (int s = 0; s < INVENTORY_SLOTS; ++s) {
                if (victim_e.inventory.slots[s] == release_wpn) {
                    inv_remove_weapon(victim_e.inventory, s); break;
                }
            }
            sem_post(&gs->state_mutex);
            for (int a = 0; a < NUM_ARTIFACTS; ++a) {
                if (gs->artifacts[a].weapon == release_wpn) {
                    gs->artifacts[a].state = ArtifactState::FREE;
                    gs->artifacts[a].held_by = -1; break;
                }
            }
            gs->waiting_for[deadlock_victim] = WPN_NONE;
            sem_post(&gs->artifact_mutex);
            artifact_on_remove(deadlock_victim, release_wpn);
            char buf[160];
            snprintf(buf, sizeof(buf), "DEADLOCK DETECTED: circular wait resolved. %s forced to release %s.",
                     victim_e.name, weapon_def(release_wpn).name);
            game_log(buf);
        } else {
            sem_post(&gs->artifact_mutex);
        }
    }
    return nullptr;
}

static bool check_end_conditions()
{
    if (g_quit) { gs->result = GameResult::QUIT; gs->phase = GamePhase::GAME_OVER; return true; }
    if (gs->enemies_killed >= MAX_ENEMIES_KILL) {
        gs->result = GameResult::WIN; gs->phase = GamePhase::GAME_OVER;
        game_log("VICTORY! 10 enemies defeated!"); return true;
    }
    bool any_alive = false;
    for (int i = 0; i < gs->num_players; ++i) if (gs->entities[i].alive) { any_alive = true; break; }
    if (!any_alive) {
        gs->result = GameResult::LOSE; gs->phase = GamePhase::GAME_OVER;
        game_log("DEFEAT! All players have fallen..."); return true;
    }
    return false;
}

static int scheduler_tick()
{
    sem_wait(&gs->state_mutex);
    int ready = -1;
    for (int i = 0; i < gs->total_entities; ++i) {
        Entity &e = gs->entities[i];
        if (!e.alive || e.stunned) continue;
        if (g_ultimate && e.type == EntityType::ENEMY) continue;
        e.stamina += static_cast<float>(e.speed);
        if (e.stamina > static_cast<float>(e.max_stamina)) e.stamina = static_cast<float>(e.max_stamina);
        if (e.stamina >= static_cast<float>(e.max_stamina)) if (ready < 0) ready = i;
    }
    sem_post(&gs->state_mutex);
    return ready;
}

static void arbiter_main_loop()
{
    while (!gs->render_ready && !g_quit) usleep(10000);
    struct timespec tick{1, 0};

    while (!check_end_conditions()) {
        int ready = scheduler_tick();
        if (ready < 0) { nanosleep(&tick, nullptr); continue; }

        Entity &actor = gs->entities[ready];
        { char buf[128]; snprintf(buf, sizeof(buf), ">>> %s's turn (stamina full)", actor.name); game_log(buf); }

        sem_wait(&gs->state_mutex);
        gs->current_turn = ready; gs->turn_ready = true;
        gs->npc_submitted = false; actor.action_ready = false; actor.action_done = false;
        sem_post(&gs->state_mutex);

        if (actor.type == EntityType::PLAYER) sem_post(&gs->hip_turn_sem);
        else sem_post(&gs->asp_turn_sem);

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
                actor.action_ready = true;
                sem_post(&gs->state_mutex);
                game_log("NPC turn timed out -- auto Skip");
            }
        }

        sem_wait(&gs->state_mutex);
        Action act = actor.pending_action; actor.action_done = true;
        sem_post(&gs->state_mutex);

        bool ultimate_triggered = (act.type == ActionType::ULTIMATE);
        bool stun_triggered = false;
        if ((act.type == ActionType::STRIKE || act.type == ActionType::USE_WEAPON) &&
            act.target_idx >= 0 && gs->entities[act.target_idx].alive) {
            unsigned tmp = g_seed;
            stun_triggered = (rng_range(tmp, 1, 5) == 1);
            g_seed = tmp;
        }

        apply_action(ready, act);
        if (ultimate_triggered) trigger_ultimate(ready);
        if (stun_triggered) deliver_stun(act.target_idx);

        char buf[256];
        for (int i = gs->num_players; i < gs->total_entities; ++i) {
            Entity &en = gs->entities[i];
            if (!en.alive && en.stamina >= 0.0f) {
                snprintf(buf, sizeof(buf), "%s has been defeated!", en.name);
                game_log(buf);
                en.stamina = -1.0f;
                for (int s2 = 0; s2 < INVENTORY_SLOTS; ++s2) {
                    WeaponID w = en.inventory.slots[s2];
                    if (w != WPN_NONE && weapon_def(w).is_artifact)
                        if (inv_first_slot_of(en.inventory, s2) == s2)
                            artifact_on_remove(i, w);
                }

                static const WeaponID DROP_TABLE[] = {
                    WPN_SPLINTER_STICK, WPN_VENOM_DAGGER, WPN_IRON_HALBERD,
                    WPN_FROSTBOW, WPN_OBSIDIAN_AXE, WPN_THUNDERSTAFF};
                bool enemy_held = false;
                for (int s2 = 0; s2 < INVENTORY_SLOTS; ++s2)
                    if (en.inventory.slots[s2] != WPN_NONE) { enemy_held = true; break; }

                if (!enemy_held && rng_range(g_seed, 1, 10) <= 3 && !gs->pending_drop_ready) {
                    int offer_to = ready;
                    if (gs->entities[offer_to].type != EntityType::PLAYER) {
                        offer_to = 0;
                        for (int p = 0; p < gs->num_players; ++p)
                            if (gs->entities[p].alive) { offer_to = p; break; }
                    }
                    WeaponID drop = DROP_TABLE[rng_range(g_seed, 0, 5)];
                    sem_wait(&gs->state_mutex);
                    gs->pending_drop_wpn = drop; gs->pending_drop_for = offer_to;
                    gs->pending_drop_ready = true; gs->pending_drop_done = false; gs->pending_drop_taken = false;
                    sem_post(&gs->state_mutex);
                    snprintf(buf, sizeof(buf), "%s dropped a %s! (waiting for pickup decision)", en.name, weapon_def(drop).name);
                    game_log(buf);

                    struct timespec dl{};
                    clock_gettime(CLOCK_REALTIME, &dl); dl.tv_sec += 30;
                    while (!gs->pending_drop_done) {
                        struct timespec ns{0, 20000000}; nanosleep(&ns, nullptr);
                        struct timespec now{}; clock_gettime(CLOCK_REALTIME, &now);
                        if (now.tv_sec >= dl.tv_sec) break;
                    }

                    if (gs->pending_drop_taken) {
                        inv_pickup(gs->entities[offer_to], drop);
                        artifact_on_pickup(offer_to, drop);
                        snprintf(buf, sizeof(buf), "%s picked up %s!", gs->entities[offer_to].name, weapon_def(drop).name);
                    } else {
                        for (int e2 = gs->num_players; e2 < gs->total_entities; ++e2) {
                            if (gs->entities[e2].alive) {
                                inv_pickup(gs->entities[e2], drop);
                                snprintf(buf, sizeof(buf), "An enemy picked up %s!", weapon_def(drop).name);
                                break;
                            }
                        }
                    }
                    game_log(buf);
                    sem_wait(&gs->state_mutex); gs->pending_drop_ready = false; sem_post(&gs->state_mutex);
                }
            }
        }

        if (check_end_conditions()) break;
        nanosleep(&tick, nullptr);
    }
}

/* ════════════════════════════════════════════════════════════════════
 * SFML Renderer
 * ════════════════════════════════════════════════════════════════════ */
static const unsigned WIN_W = 1100;
static const unsigned WIN_H = 780;
static const float MARGIN = 18.f;
static const float ROW_H  = 30.f;
static const float BAR_W  = 200.f;
static const float BAR_H  = 14.f;

static const sf::Color C_BG       {15, 15, 25};
static const sf::Color C_PANEL    {28, 28, 48};
static const sf::Color C_BORDER   {60, 60,100};
static const sf::Color C_TITLE    {220,200,100};
static const sf::Color C_WHITE    {230,230,230};
static const sf::Color C_ACTIVE   {255,230, 80};
static const sf::Color C_DEAD     {100, 60, 60};
static const sf::Color C_STUN     {180,100,255};
static const sf::Color C_HP_PLAYER{ 60,200, 80};
static const sf::Color C_HP_ENEMY {220, 60, 60};
static const sf::Color C_STAMINA  { 60,180,255};
static const sf::Color C_LOG_BG   { 20, 20, 35};
static const sf::Color C_LOG_TEXT {180,200,160};
static const sf::Color C_ARTIFACT {255,200, 80};
static const sf::Color C_ULTIMATE {255,100,200};
static const sf::Color C_SECTION  { 80, 80,140};

static void draw_bar_sfml(sf::RenderTarget &rt,
                          float x, float y, float w, float h,
                          float val, float max_val, sf::Color filled_col)
{
    sf::RectangleShape bg({w,h}); bg.setPosition(x,y);
    bg.setFillColor({40,40,60}); bg.setOutlineColor(C_BORDER); bg.setOutlineThickness(1.f);
    rt.draw(bg);
    if (max_val > 0.f) {
        float ratio = val / max_val;
        if (ratio < 0.f) ratio = 0.f; if (ratio > 1.f) ratio = 1.f;
        sf::RectangleShape fill({w*ratio,h}); fill.setPosition(x,y); fill.setFillColor(filled_col);
        rt.draw(fill);
    }
}

static void draw_panel(sf::RenderTarget &rt, float x, float y, float w, float h)
{
    sf::RectangleShape panel({w,h}); panel.setPosition(x,y);
    panel.setFillColor(C_PANEL); panel.setOutlineColor(C_BORDER); panel.setOutlineThickness(1.5f);
    rt.draw(panel);
}

static void draw_section_line(sf::RenderTarget &rt, float x, float y, float w,
                              const std::string &label, const sf::Font &font)
{
    sf::RectangleShape line({w,1.5f}); line.setPosition(x,y+10.f); line.setFillColor(C_SECTION); rt.draw(line);
    sf::Text t(label,font,13); t.setFillColor(C_SECTION); t.setPosition(x+4.f,y-1.f); rt.draw(t);
}

/* ── Entity row with pixel-art sprite ── */
static void render_entity_row_sfml(sf::RenderTarget &rt,
                                   float x, float y, float panel_w,
                                   const Entity &e, bool active,
                                   const sf::Font &font)
{
    /* Row highlight */
    if (active) {
        sf::RectangleShape hl({panel_w-4.f, ROW_H-2.f});
        hl.setPosition(x+2.f, y); hl.setFillColor({40,40,10,80}); rt.draw(hl);
    }

    /* Sprite */
    float sx = x + 6.f;
    float sy = y + (ROW_H - SPRITE_H) / 2.f - 1.f;

    if (!e.alive)
        draw_dead_sprite(rt, sx, sy);
    else if (e.stunned)
        draw_stun_sprite(rt, sx, sy);
    else if (e.type == EntityType::PLAYER)
        draw_player_sprite(rt, sx, sy, {220,185,80});  // gold knight
    else
        draw_enemy_sprite(rt, sx, sy, {200,80,40});    // orange-red skull

    float name_x = x + 6.f + SPRITE_W + 4.f;

    /* Name */
    sf::Color name_col = active    ? C_ACTIVE
                       : !e.alive  ? C_DEAD
                       : e.stunned ? C_STUN
                                   : C_WHITE;
    sf::Text name_t(e.name, font, 13);
    name_t.setFillColor(name_col);
    name_t.setPosition(name_x, y + 6.f);
    if (active) name_t.setStyle(sf::Text::Bold);
    rt.draw(name_t);

    /* Status badge */
    const char *badge = !e.alive ? "[DEAD]" : e.stunned ? "[STUN]" : nullptr;
    if (badge) {
        sf::Text bt(badge, font, 11); bt.setFillColor(e.stunned ? C_STUN : C_DEAD);
        bt.setPosition(x + 130.f, y + 8.f); rt.draw(bt);
    }

    /* HP bar */
    float bx = x + 185.f;
    draw_bar_sfml(rt, bx, y+8.f, BAR_W, BAR_H,
                  (float)e.hp, (float)e.max_hp,
                  e.type==EntityType::PLAYER ? C_HP_PLAYER : C_HP_ENEMY);
    std::ostringstream hpss; hpss << e.hp << "/" << e.max_hp;
    sf::Text hp_t(hpss.str(), font, 11); hp_t.setFillColor(C_WHITE);
    hp_t.setPosition(bx+BAR_W+4.f, y+7.f); rt.draw(hp_t);

    /* Stamina bar */
    float stx = bx + BAR_W + 70.f;
    float st_val = e.stamina < 0.f ? 0.f : e.stamina;
    draw_bar_sfml(rt, stx, y+8.f, 120.f, BAR_H, st_val, (float)e.max_stamina, C_STAMINA);
    std::ostringstream stss; stss << (int)st_val << "/" << e.max_stamina;
    sf::Text st_t(stss.str(), font, 11); st_t.setFillColor(C_WHITE);
    st_t.setPosition(stx+124.f, y+7.f); rt.draw(st_t);
}

static void *render_thread_fn(void *)
{
    sf::RenderWindow window(sf::VideoMode(WIN_W, WIN_H), "  CHRONO RIFT ",
                            sf::Style::Titlebar | sf::Style::Close);
    window.setFramerateLimit(10);
    gs->render_ready = true;

    sf::Font font;
    const char *font_paths[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/truetype/ubuntu/UbuntuMono-R.ttf",
        "/usr/share/fonts/truetype/freefont/FreeMono.ttf", nullptr};
    for (int i = 0; font_paths[i]; ++i) if (font.loadFromFile(font_paths[i])) break;

    SharedState snap{};

    /* Splash inside game window */
    {
        sf::Clock splash_clk;
        const float SPLASH_SECS = 2.5f;
        while (window.isOpen() && !g_quit && splash_clk.getElapsedTime().asSeconds() < SPLASH_SECS) {
            sf::Event ev{};
            while (window.pollEvent(ev)) {
                if (ev.type == sf::Event::Closed) { g_quit = 1; window.close(); }
                if (ev.type == sf::Event::KeyPressed || ev.type == sf::Event::MouseButtonPressed) goto splash_done;
            }
            float t = splash_clk.getElapsedTime().asSeconds();
            float alpha = 1.f;
            if (t < 0.6f) alpha = t / 0.6f;
            else if (t > SPLASH_SECS - 0.5f) alpha = (SPLASH_SECS - t) / 0.5f;
            if (alpha < 0.f) alpha = 0.f; if (alpha > 1.f) alpha = 1.f;
            sf::Uint8 a = (sf::Uint8)(alpha * 255);

            window.clear(sf::Color{4,6,14});
            sf::Text title("CHRONO RIFT", font, 72);
            title.setFillColor({220,195,80,a}); title.setStyle(sf::Text::Bold);
            sf::FloatRect tb = title.getLocalBounds();
            title.setOrigin(tb.left+tb.width/2.f, tb.top+tb.height/2.f);
            title.setPosition(WIN_W/2.f, WIN_H/2.f-10.f);
            window.draw(title);
            window.display();
        }
        splash_done:;
    }

    while (window.isOpen() && !g_quit && !g_render_done) {
        sf::Event ev{};
        while (window.pollEvent(ev)) {
            if (ev.type == sf::Event::Closed) { g_quit = 1; window.close(); }
        }

        sem_wait(&gs->state_mutex);
        memcpy(&snap, gs, sizeof(snap));
        sem_post(&gs->state_mutex);

        window.clear(C_BG);
        float cx = MARGIN, cy = MARGIN;

        {
            const char *phase_str = snap.phase==GamePhase::ULTIMATE_PAUSE ? "  *** ULTIMATE PAUSE ***"
                                  : snap.phase==GamePhase::GAME_OVER      ? "  GAME OVER" : "  RUNNING";
            sf::Color phase_col   = snap.phase==GamePhase::ULTIMATE_PAUSE ? C_ULTIMATE
                                  : snap.phase==GamePhase::GAME_OVER      ? C_HP_ENEMY : C_HP_PLAYER;
            std::ostringstream title;
            title << "CHRONO RIFT      Kills : " << snap.enemies_killed << " / " << MAX_ENEMIES_KILL << phase_str;
            sf::Text t(title.str(), font, 16); t.setFillColor(phase_col); t.setStyle(sf::Text::Bold);
            t.setPosition(cx, cy); window.draw(t);
        }
        cy += 30.f;

        float left_w = WIN_W * 0.58f;
        float right_x = cx + left_w + 12.f;
        float right_w = WIN_W - right_x - MARGIN;
        float top_y = cy;

        float players_h = MARGIN + snap.num_players * ROW_H + MARGIN;
        draw_panel(window, cx, cy, left_w, players_h);
        draw_section_line(window, cx+8.f, cy+6.f, left_w-16.f, "PLAYERS", font);
        { sf::Text hdr("      Name                    Status  HP Bar        HP        Stamina Bar      ST",font,11);
          hdr.setFillColor(C_SECTION); hdr.setPosition(cx+6.f,cy+18.f); window.draw(hdr); }
        float ey = cy + MARGIN + 12.f;
        for (int i = 0; i < snap.num_players; ++i, ey += ROW_H)
            render_entity_row_sfml(window, cx, ey, left_w, snap.entities[i], snap.current_turn==i, font);
        cy += players_h + 10.f;

        float enemies_h = MARGIN + snap.num_enemies * ROW_H + MARGIN;
        draw_panel(window, cx, cy, left_w, enemies_h);
        draw_section_line(window, cx+8.f, cy+6.f, left_w-16.f, "ENEMIES", font);
        ey = cy + MARGIN + 12.f;
        for (int i = 0; i < snap.num_enemies; ++i, ey += ROW_H) {
            int eidx = snap.num_players + i;
            render_entity_row_sfml(window, cx, ey, left_w, snap.entities[eidx], snap.current_turn==eidx, font);
        }
        cy += enemies_h + 10.f;

        float ry = top_y;
        int artifact_count = 0;
        for (int a = 0; a < NUM_ARTIFACTS; ++a) if (snap.artifacts[a].exists) ++artifact_count;
        float art_h = MARGIN + artifact_count * ROW_H + MARGIN;
        draw_panel(window, right_x, ry, right_w, art_h);
        draw_section_line(window, right_x+8.f, ry+6.f, right_w-16.f, "ARTIFACTS", font);
        float ay = ry + MARGIN + 12.f;
        for (int a = 0; a < NUM_ARTIFACTS; ++a) {
            const ArtifactEntry &ae = snap.artifacts[a];
            if (!ae.exists) continue;
            const char *holder_name = (ae.state==ArtifactState::HELD && ae.held_by>=0) ? snap.entities[ae.held_by].name : "FREE";
            sf::Color ac = (ae.state==ArtifactState::HELD) ? C_ARTIFACT : C_SECTION;
            std::ostringstream aoss; aoss << weapon_def(ae.weapon).name << " : " << holder_name;
            sf::Text at(aoss.str(),font,13); at.setFillColor(ac); at.setPosition(right_x+8.f,ay+4.f); window.draw(at);
            ay += ROW_H;
        }
        ry += art_h + 10.f;

        float log_h = WIN_H - ry - MARGIN - 10.f; if (log_h < 60.f) log_h = 60.f;
        draw_panel(window, right_x, ry, right_w, log_h);
        draw_section_line(window, right_x+8.f, ry+6.f, right_w-16.f, "ACTION LOG", font);
        int max_lines = (int)((log_h - MARGIN - 12.f) / 18.f);
        if (max_lines > LOG_LINES) max_lines = LOG_LINES;
        float ly = ry + MARGIN + 12.f;
        for (int l = 0; l < max_lines; ++l) {
            int idx = ((snap.log_head - max_lines + l + LOG_LINES) % LOG_LINES);
            if (snap.log[idx][0] == '\0') continue;
            sf::Color lc = C_LOG_TEXT;
            std::string line_str = snap.log[idx];
            if (line_str.find("ULTIMATE") != std::string::npos) lc = C_ULTIMATE;
            else if (line_str.find("STUN") != std::string::npos) lc = C_STUN;
            else if (line_str.find("DEADLOCK") != std::string::npos) lc = C_HP_ENEMY;
            else if (line_str.find("DEFEAT") != std::string::npos) lc = C_HP_ENEMY;
            else if (line_str.find("VICTORY") != std::string::npos) lc = C_ACTIVE;
            if (line_str.size() > 42) line_str = line_str.substr(0,39) + "...";
            sf::Text lt(line_str,font,12); lt.setFillColor(lc); lt.setPosition(right_x+8.f,ly); window.draw(lt);
            ly += 18.f;
        }

        if (snap.phase == GamePhase::GAME_OVER) {
            sf::RectangleShape overlay({(float)WIN_W,(float)WIN_H}); overlay.setFillColor({0,0,0,160}); window.draw(overlay);
            const char *msg = snap.result==GameResult::WIN ? "VICTORY!" : snap.result==GameResult::LOSE ? "DEFEAT!" : "GAME QUIT";
            sf::Color mc = (snap.result==GameResult::WIN) ? C_ACTIVE : C_HP_ENEMY;
            sf::Text mt(msg,font,56); mt.setFillColor(mc); mt.setStyle(sf::Text::Bold);
            sf::FloatRect mb=mt.getLocalBounds(); mt.setOrigin(mb.width/2.f,mb.height/2.f);
            mt.setPosition(WIN_W/2.f,WIN_H/2.f-30.f); window.draw(mt);
            sf::Text sub("Window will close in a moment...",font,18); sub.setFillColor(C_WHITE);
            sf::FloatRect sb=sub.getLocalBounds(); sub.setOrigin(sb.width/2.f,sb.height/2.f);
            sub.setPosition(WIN_W/2.f,WIN_H/2.f+40.f); window.draw(sub);
        }

        window.display();
        if (snap.phase == GamePhase::GAME_OVER) { sf::sleep(sf::seconds(3.f)); window.close(); }
    }

    if (window.isOpen()) window.close();
    return nullptr;
}

int arbiter_main(int argc, char *argv[])
{
    if (argc < 3) { fprintf(stderr, "Usage : arbiter <roll_number> <num_players>\n"); return 1; }

    unsigned roll_no = (unsigned)atoi(argv[1]);
    int num_players = atoi(argv[2]);
    if (num_players < 1) num_players = 1;
    if (num_players > 4) num_players = 4;

    g_seed = roll_no;
    int num_enemies = rng_range(g_seed, 2, 9);

    printf("[Arbiter] seed=%u | players=%d | enemies=%d\n", roll_no, num_players, num_enemies);
    fflush(stdout);

    gs = shm_create();
    init_shared_state(num_players, num_enemies, roll_no);

    struct sigaction sa{};
    sa.sa_handler = sig_term; sigaction(SIGTERM, &sa, nullptr);
    sa.sa_handler = sig_alrm; sigaction(SIGALRM, &sa, nullptr);

    pthread_t render_tid; pthread_create(&render_tid, nullptr, render_thread_fn, nullptr);
    pthread_t dl_tid;     pthread_create(&dl_tid, nullptr, deadlock_monitor, nullptr);

    char roll_str[16], np_str[8];
    snprintf(roll_str, sizeof(roll_str), "%u", roll_no);
    snprintf(np_str,   sizeof(np_str),   "%d", num_players);

    g_hip_pid = fork();
    if (g_hip_pid == 0) { execlp("./hip_bin","./hip_bin",roll_str,np_str,nullptr); perror("exec hip"); _exit(1); }
    gs->hip_pid = g_hip_pid;

    char ne_str[8]; snprintf(ne_str, sizeof(ne_str), "%d", num_enemies);
    g_asp_pid = fork();
    if (g_asp_pid == 0) { execlp("./asp_bin","./asp_bin",roll_str,np_str,ne_str,nullptr); perror("exec asp"); _exit(1); }
    gs->asp_pid = g_asp_pid;

    arbiter_main_loop();

    g_quit = 1; g_render_done = 1;
    if (g_hip_pid > 0) kill(g_hip_pid, SIGTERM);
    if (g_asp_pid > 0) { kill(g_asp_pid, SIGCONT); kill(g_asp_pid, SIGTERM); }
    waitpid(g_hip_pid, nullptr, 0); waitpid(g_asp_pid, nullptr, 0);
    pthread_join(render_tid, nullptr); pthread_join(dl_tid, nullptr);

    sem_destroy(&gs->state_mutex); sem_destroy(&gs->action_sem);
    sem_destroy(&gs->hip_turn_sem); sem_destroy(&gs->asp_turn_sem);
    sem_destroy(&gs->artifact_mutex); sem_destroy(&gs->log_mutex);

    switch (gs->result) {
    case GameResult::WIN:  printf("\n VICTORY! You defeated %d enemies! \n", gs->enemies_killed); break;
    case GameResult::LOSE: printf("\n DEFEAT! All players have fallen. \n"); break;
    case GameResult::QUIT: printf("\n Game quit by player. \n"); break;
    default: break;
    }

    shm_destroy(); return 0;
}
