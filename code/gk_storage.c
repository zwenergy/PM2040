// GK mini flash storage, filled in by the multi-ROM patcher.
//
// Kept in its own translation unit on purpose: code elsewhere must not see
// the initializers, or the compiler would constant-fold the unpatched
// contents. The patcher finds the markers; the BIOS marker is overwritten
// by the BIOS itself, the slot table keeps its magic.
#include <stdint.h>

const uint8_t gk_bios_storage[524288]
	__attribute__((section(".gkStorage"), aligned(4096))) = {
	'G', 'K', 'B', 'I', 'O', 'S', 'S', 'T' };

const uint8_t gk_slot_table_storage[4096]
	__attribute__((section(".gkStorage"), aligned(4096))) = {
	'G', 'K', 'S', 'L', 'O', 'T', 'T', 'B' };
