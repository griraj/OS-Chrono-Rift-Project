#pragma once
/*
 * inventory.h
 * Contiguous-fit inventory allocator.
 *
 * Primary inventory: 20-slot linear array.
 * Weapons occupy contiguous slots.
 * On overflow: swap out minimum weapons to LTS to free space.
 * Solar Core + Lunar Blade = exactly 20 slots (10+10) — by design.
 * Splinter Stick (2 slots) exercises fragmentation edge cases.
 */

#include "shared_state.h"

/* Find first index where `size` contiguous free slots exist. Returns -1 if none. */
int inv_find_fit(const Inventory& inv, int size);

/* Place weapon at `start` for `size` slots. */
void inv_add_weapon(Inventory& inv, WeaponID id, int start, int size);

/* Remove weapon whose first slot is at `start`. Returns the WeaponID removed. */
WeaponID inv_remove_weapon(Inventory& inv, int start);

/* Given any occupied slot, return the index of the first slot of that weapon's run. */
int inv_first_slot_of(const Inventory& inv, int slot);

/* High-level pickup: place weapon in primary inventory, swapping to LTS if needed.
 * Returns true on success. */
bool inv_pickup(Entity& e, WeaponID id);

/* Swap a weapon from LTS back into primary inventory.
 * Returns true on success. */
bool inv_swap_in(Entity& e, WeaponID id);

/* Returns true if weapon `id` is present anywhere in primary inventory. */
bool inv_has_weapon(const Entity& e, WeaponID id);
