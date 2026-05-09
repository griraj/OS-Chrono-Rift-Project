
#include "inventory.h"
#include <cstring>
#include <climits>

static bool is_first_slot(const Inventory &inv, int i)
{
    if (inv.slots[i] == WPN_NONE)
        return false;
    if (i == 0)
        return true;
    return inv.slots[i - 1] != inv.slots[i];
}

static int free_run_length(const Inventory &inv, int start)
{
    int len = 0;
    for (int i = start; i < INVENTORY_SLOTS && inv.slots[i] == WPN_NONE; ++i)
        ++len;
    return len;
}

int inv_find_fit(const Inventory &inv, int size)
{
    for (int i = 0; i <= INVENTORY_SLOTS - size; ++i)
        if (free_run_length(inv, i) >= size)
            return i;
    return -1;
}

void inv_add_weapon(Inventory &inv, WeaponID id, int start, int size)
{
    for (int i = start; i < start + size; ++i)
        inv.slots[i] = id;
}

WeaponID inv_remove_weapon(Inventory &inv, int start)
{
    if (start < 0 || start >= INVENTORY_SLOTS)
        return WPN_NONE;
    WeaponID id = inv.slots[start];
    if (id == WPN_NONE)
        return WPN_NONE;
    for (int i = start; i < INVENTORY_SLOTS && inv.slots[i] == id; ++i)
        inv.slots[i] = WPN_NONE;
    return id;
}

int inv_first_slot_of(const Inventory &inv, int slot)
{
    if (slot < 0 || slot >= INVENTORY_SLOTS)
        return -1;
    WeaponID id = inv.slots[slot];
    if (id == WPN_NONE)
        return -1;
    int s = slot;
    while (s > 0 && inv.slots[s - 1] == id)
        --s;
    return s;
}

bool inv_pickup(Entity &e, WeaponID id)
{
    const int need = weapon_def(id).slots;
    if (need <= 0 || need > INVENTORY_SLOTS)
        return false;

    int fit = inv_find_fit(e.inventory, need);
    if (fit >= 0)
    {
        inv_add_weapon(e.inventory, id, fit, need);
        return true;
    }

    Inventory tmp_inv = e.inventory;
    LongTermStorage tmp_lts = e.lts;

    for (int i = 0; i < INVENTORY_SLOTS; ++i)
    {
        if (!is_first_slot(tmp_inv, i))
            continue;
        WeaponID removed = inv_remove_weapon(tmp_inv, i);
        if (tmp_lts.count < MAX_LTS_WEAPONS)
            tmp_lts.weapons[tmp_lts.count++] = removed;

        fit = inv_find_fit(tmp_inv, need);
        if (fit >= 0)
        {
            e.inventory = tmp_inv;
            e.lts = tmp_lts;
            inv_add_weapon(e.inventory, id, fit, need);
            return true;
        }
    }
    return false;
}

bool inv_swap_in(Entity &e, WeaponID id)
{
    int found = -1;
    for (int i = 0; i < e.lts.count; ++i)
        if (e.lts.weapons[i] == id)
        {
            found = i;
            break;
        }
    if (found < 0)
        return false;

    for (int i = found; i < e.lts.count - 1; ++i)
        e.lts.weapons[i] = e.lts.weapons[i + 1];
    --e.lts.count;

    return inv_pickup(e, id);
}

bool inv_has_weapon(const Entity &e, WeaponID id)
{
    for (int i = 0; i < INVENTORY_SLOTS; ++i)
        if (e.inventory.slots[i] == id)
            return true;
    return false;
}
