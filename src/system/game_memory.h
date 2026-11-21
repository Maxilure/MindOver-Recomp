// game_memory.h -- see game_memory.cpp (the game's main heap in virtual
// memory, full heaps growing into the port's pool, refused allocations named).
#pragma once

#include <cstdint>
#include <string>

namespace game_memory {

// --debug_physical_callers (graphics memory research, docs/findings/71 s.4
// step 3): every XPhysicalAlloc by its callers; console `memory physical`.
void NotePhysical(uint32_t address, uint32_t bytes, uint32_t lr, uint32_t protect, uint32_t r1);
void NotePhysicalFree(uint32_t address);
std::string PhysicalReport();

}  // namespace game_memory
