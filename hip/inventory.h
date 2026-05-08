#pragma once

#include "shared_state.h"

int inv_find_fit(const Inventory &inv, int size);

void inv_add_weapon(Inventory &inv, WeaponID id, int start, int size);

WeaponID inv_remove_weapon(Inventory &inv, int start);

int inv_first_slot_of(const Inventory &inv, int slot);

bool inv_pickup(Entity &e, WeaponID id);

bool inv_swap_in(Entity &e, WeaponID id);

bool inv_has_weapon(const Entity &e, WeaponID id);
