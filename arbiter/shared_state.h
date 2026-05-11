#pragma once

#include <semaphore.h>
#include <sys/types.h>
#include <cstdint>
#include <cstring>

constexpr int MAX_PLAYERS = 4;
constexpr int MAX_ENEMIES = 9;
constexpr int MAX_ENTITIES = MAX_PLAYERS + MAX_ENEMIES;
constexpr int INVENTORY_SLOTS = 20;
constexpr int MAX_LTS_WEAPONS = 64;
constexpr int MAX_ENEMIES_KILL = 10;
constexpr int NPC_TURN_TIMEOUT = 3;
constexpr int STUN_DURATION = 3;
constexpr int ULTIMATE_DURATION = 10;
constexpr int NUM_ARTIFACTS = 3;
constexpr int LOG_LINES = 16;
constexpr int LOG_LINE_LEN = 256;
constexpr const char *SHM_NAME = "/chrono_rift_shm";

enum WeaponID : int
{
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

struct WeaponDef
{
    WeaponID id;
    char name[32];
    int slots;
    int damage;
    bool is_artifact;
};

inline const WeaponDef &weapon_def(WeaponID id)
{
    static const WeaponDef TABLE[WPN_COUNT] = {
        {WPN_NONE, "None", 0, 0, false},
        {WPN_SOLAR_CORE, "Solar Core", 10, 95, true},
        {WPN_LUNAR_BLADE, "Lunar Blade", 10, 90, true},
        {WPN_IRON_HALBERD, "Iron Halberd", 7, 55, false},
        {WPN_VENOM_DAGGER, "Venom Dagger", 4, 30, false},
        {WPN_THUNDERSTAFF, "Thunderstaff", 6, 50, false},
        {WPN_OBSIDIAN_AXE, "Obsidian Axe", 5, 45, false},
        {WPN_FROSTBOW, "Frostbow", 6, 48, false},
        {WPN_SPLINTER_STICK, "Splinter Stick", 2, 12, false},
        {WPN_ECLIPSE_RELIC, "Eclipse Relic", 5, 60, true},
    };
    return TABLE[id];
}

struct Inventory
{
    WeaponID slots[INVENTORY_SLOTS];
    Inventory()
    {
        memset(slots, 0, sizeof(slots));
    }
};

struct LongTermStorage
{
    WeaponID weapons[MAX_LTS_WEAPONS];
    int count = 0;
};

enum class ActionType : int
{
    NONE = 0,
    STRIKE,
    EXHAUST,
    USE_WEAPON,
    SWAP_IN,
    HEAL,
    SKIP,
    ULTIMATE
};

struct Action
{
    ActionType type = ActionType::NONE;
    int target_idx = -1;
    int weapon_slot = -1;
    WeaponID swap_wpn = WPN_NONE;
};

enum class EntityType : int
{
    PLAYER = 0,
    ENEMY
};

struct Entity
{
    int idx = 0;
    EntityType type = EntityType::PLAYER;
    char name[32] = {};
    pid_t pid = 0;
    int thread_id = 0;

    int hp = 0;
    int max_hp = 0;
    int damage = 0;
    int speed = 0;
    int max_stamina = 0;
    float stamina = 0.0f;
    bool alive = true;
    bool stunned = false;

    Inventory inventory;
    LongTermStorage lts;
    WeaponID held_loot = WPN_NONE;

    Action pending_action;
    bool action_ready = false;
    bool action_done = false;
    int swap_in_slot = -1;

    char last_log[128] = {};
};

enum class ArtifactState : int
{
    FREE = 0,
    HELD
};

struct ArtifactEntry
{
    WeaponID weapon = WPN_NONE;
    ArtifactState state = ArtifactState::FREE;
    int held_by = -1;
    bool exists = false;
};

enum class GamePhase : int
{
    INIT = 0,
    RUNNING,
    ULTIMATE_PAUSE,
    GAME_OVER
};
enum class GameResult : int
{
    NONE = 0,
    WIN,
    LOSE,
    QUIT
};

struct SharedState
{
    Entity entities[MAX_ENTITIES];
    int num_players = 0;
    int num_enemies = 0;
    int total_entities = 0;

    GamePhase phase = GamePhase::INIT;
    GameResult result = GameResult::NONE;
    int enemies_killed = 0;
    int current_turn = -1;
    bool turn_ready = false;
    bool npc_submitted = false;

    ArtifactEntry artifacts[NUM_ARTIFACTS];

    WeaponID waiting_for[MAX_ENTITIES];

    char log[LOG_LINES][LOG_LINE_LEN];
    int log_head = 0;

    sem_t state_mutex;
    sem_t action_sem;
    sem_t hip_turn_sem;
    sem_t asp_turn_sem;
    sem_t artifact_mutex;
    sem_t log_mutex;

    volatile bool render_ready = false;

    int num_hip_procs = 0;
    pid_t hip_pids[MAX_PLAYERS] = {};
    pid_t arbiter_pid = 0;
    pid_t hip_pid = 0;
    pid_t asp_pid = 0;

    unsigned seed = 0;

    volatile bool pending_drop_ready = false;
    volatile bool pending_drop_done = false;
    volatile bool pending_drop_taken = false;
    WeaponID pending_drop_wpn = WPN_NONE;
    int pending_drop_for = -1;

    int active_players[MAX_PLAYERS];
    int num_active_players = 0;
    int active_enemies[MAX_ENEMIES];
    int num_active_enemies = 0;
};

// Return a pseudo-random integer within [lo, hi] using seed.
inline int rng_range(unsigned &seed, int lo, int hi)
{
    seed = seed * 1664525u + 1013904223u;
    return lo + static_cast<int>((seed >> 16) % static_cast<unsigned>(hi - lo + 1));
}
