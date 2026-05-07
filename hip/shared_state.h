#pragma once
/*
 * shared_state.h
 * Shared memory layout for Chrono Rift.
 * This header is included by all three processes (arbiter, hip, asp).
 * All IPC is done exclusively through this shared memory region.
 * NO pipes (named or unnamed) are used anywhere.
 */

#include <semaphore.h>
#include <sys/types.h>
#include <cstdint>
#include <cstring>

/* ─── Configuration ──────────────────────────────────────────────── */
constexpr int  MAX_PLAYERS       = 4;
constexpr int  MAX_ENEMIES       = 9;
constexpr int  MAX_ENTITIES      = MAX_PLAYERS + MAX_ENEMIES;
constexpr int  INVENTORY_SLOTS   = 20;
constexpr int  MAX_LTS_WEAPONS   = 64;
constexpr int  MAX_ENEMIES_KILL  = 10;
constexpr int  NPC_TURN_TIMEOUT  = 3;   /* seconds */
constexpr int  STUN_DURATION     = 3;   /* seconds */
constexpr int  ULTIMATE_DURATION = 10;  /* seconds */
constexpr int  NUM_ARTIFACTS     = 3;
constexpr int  LOG_LINES         = 16;
constexpr int  LOG_LINE_LEN      = 256;
constexpr const char* SHM_NAME  = "/chrono_rift_shm";

/* ─── Weapon IDs ─────────────────────────────────────────────────── */
enum WeaponID : int {
    WPN_NONE = 0,
    WPN_SOLAR_CORE,
    WPN_LUNAR_BLADE,
    WPN_IRON_HALBERD,
    WPN_VENOM_DAGGER,
    WPN_THUNDERSTAFF,
    WPN_OBSIDIAN_AXE,
    WPN_FROSTBOW,
    WPN_SPLINTER_STICK,
    WPN_ECLIPSE_RELIC,
    WPN_COUNT
};

struct WeaponDef {
    WeaponID    id;
    char        name[32];
    int         slots;
    int         damage;
    bool        is_artifact;
};

/* Weapon table — indexed by WeaponID */
inline const WeaponDef& weapon_def(WeaponID id)
{
    static const WeaponDef TABLE[WPN_COUNT] = {
        {WPN_NONE,          "None",           0,  0,  false},
        {WPN_SOLAR_CORE,    "Solar Core",     10, 95, true },
        {WPN_LUNAR_BLADE,   "Lunar Blade",    10, 90, true },
        {WPN_IRON_HALBERD,  "Iron Halberd",    7, 55, false},
        {WPN_VENOM_DAGGER,  "Venom Dagger",    4, 30, false},
        {WPN_THUNDERSTAFF,  "Thunderstaff",    6, 50, false},
        {WPN_OBSIDIAN_AXE,  "Obsidian Axe",    5, 45, false},
        {WPN_FROSTBOW,      "Frostbow",        6, 48, false},
        {WPN_SPLINTER_STICK,"Splinter Stick",  2, 12, false},
        {WPN_ECLIPSE_RELIC, "Eclipse Relic",   5, 60, true },
    };
    return TABLE[id];
}

/* ─── Inventory ──────────────────────────────────────────────────── */
struct Inventory {
    WeaponID slots[INVENTORY_SLOTS];
    Inventory() { memset(slots, 0, sizeof(slots)); }
};

struct LongTermStorage {
    WeaponID weapons[MAX_LTS_WEAPONS];
    int      count = 0;
};

/* ─── Action ─────────────────────────────────────────────────────── */
enum class ActionType : int {
    NONE = 0,
    STRIKE,
    EXHAUST,
    USE_WEAPON,
    SWAP_IN,
    HEAL,
    SKIP,
    ULTIMATE       /* USE_WEAPON while holding both artifacts */
};

struct Action {
    ActionType type       = ActionType::NONE;
    int        target_idx = -1;
    int        weapon_slot= -1;
    WeaponID   swap_wpn   = WPN_NONE;
};

/* ─── Entity ─────────────────────────────────────────────────────── */
enum class EntityType : int { PLAYER = 0, ENEMY };

struct Entity {
    int        idx         = 0;
    EntityType type        = EntityType::PLAYER;
    char       name[32]    = {};
    pid_t      pid         = 0;
    int        thread_id   = 0;

    /* Combat stats */
    int        hp          = 0;
    int        max_hp      = 0;
    int        damage      = 0;
    int        speed       = 0;
    int        max_stamina = 0;
    float      stamina     = 0.0f;
    bool       alive       = true;
    bool       stunned     = false;

    /* Inventory (players only) */
    Inventory       inventory;
    LongTermStorage lts;

    /* Action mailbox: hip/asp writes, arbiter reads */
    Action     pending_action;
    bool       action_ready = false;   /* set by hip/asp after writing action */
    bool       action_done  = false;   /* set by arbiter after consuming */

    char       last_log[128] = {};
};

/* ─── Artifact / Resource Table ─────────────────────────────────── */
enum class ArtifactState : int { FREE = 0, HELD };

struct ArtifactEntry {
    WeaponID      weapon   = WPN_NONE;
    ArtifactState state    = ArtifactState::FREE;
    int           held_by  = -1;
    bool          exists   = false;
};

/* ─── Game phase / result ────────────────────────────────────────── */
enum class GamePhase  : int { INIT = 0, RUNNING, ULTIMATE_PAUSE, GAME_OVER };
enum class GameResult : int { NONE = 0, WIN, LOSE, QUIT };

/* ─── Full shared state ──────────────────────────────────────────── */
struct SharedState {
    /* Entities */
    Entity      entities[MAX_ENTITIES];
    int         num_players    = 0;
    int         num_enemies    = 0;
    int         total_entities = 0;

    /* Game status */
    GamePhase   phase          = GamePhase::INIT;
    GameResult  result         = GameResult::NONE;
    int         enemies_killed = 0;
    int         current_turn   = -1;
    bool        turn_ready     = false;
    bool        npc_submitted  = false;

    /* Artifacts */
    ArtifactEntry artifacts[NUM_ARTIFACTS];

    /* Action log (ring buffer, 16 lines) */
    char        log[LOG_LINES][LOG_LINE_LEN];
    int         log_head = 0;

    /* ── Synchronisation (memory-based, no pipes) ── */
    sem_t state_mutex;      /* protects full state  (pshared=1, init=1) */
    sem_t action_sem;       /* arbiter waits; entity posts when ready    */
    sem_t hip_turn_sem;     /* hip dispatcher waits; arbiter posts on PLAYER turn */
    sem_t asp_turn_sem;     /* asp dispatcher waits; arbiter posts on ENEMY turn  */
    sem_t artifact_mutex;   /* protects artifact table                   */
    sem_t log_mutex;        /* protects log ring buffer                  */

    /* Process PIDs for signal delivery */
    pid_t arbiter_pid   = 0;
    pid_t hip_pid       = 0;
    pid_t asp_pid       = 0;

    /* Weapon drop: arbiter sets after enemy death,
     * hip shows pickup prompt, then clears it */
    WeaponID pending_drop_wpn   = WPN_NONE;
    int      pending_drop_for   = -1;    // player entity idx offered the drop
    bool     pending_drop_ready = false; // arbiter→hip: drop is waiting
    bool     pending_drop_done  = false; // hip→arbiter: player responded
    bool     pending_drop_taken = false; // hip→arbiter: player picked it up

    /* Roll number seed */
    unsigned seed = 0;
};

/* ─── RNG (LCG seeded from roll number) ─────────────────────────── */
inline int rng_range(unsigned& seed, int lo, int hi)
{
    seed = seed * 1664525u + 1013904223u;
    return lo + static_cast<int>((seed >> 16) % static_cast<unsigned>(hi - lo + 1));
}
