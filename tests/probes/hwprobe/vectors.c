// Installing and restoring the exception vectors (hwp.h).
#include <libdragon.h>
#include <string.h>

#include "hwp.h"

extern uint32_t hwp_vec_stub[4], hwp_vec_minimal[4];
static uint32_t saved[0x200 / 4];
static bool installed;

static void sync_vectors(void) {
  data_cache_hit_writeback((void*)0x80000000u, 0x200);
  inst_cache_hit_invalidate((void*)0x80000000u, 0x200);
}

void hwp_vectors_install(void) {
  if (!installed) memcpy(saved, (void*)0x80000000u, sizeof saved);
  installed = true;
  for (uint32_t v = 0; v < 0x200; v += 0x80) memcpy((void*)(0x80000000u + v), hwp_vec_stub, 16);
  sync_vectors();
}

void hwp_vectors_restore(void) {
  if (!installed) return;
  memcpy((void*)0x80000000u, saved, sizeof saved);
  sync_vectors();
}

void hwp_vector180_minimal(bool on) { hwp_vector_minimal_at(0x180, on); }

void hwp_vector_minimal_at(uint32_t vec, bool on) {
  memcpy((void*)(0x80000000u + vec), on ? hwp_vec_minimal : hwp_vec_stub, 16);
  sync_vectors();
}

void hwp_exc_reset(uint32_t expect) {
  hwp_exc.n = 0;
  hwp_exc.expect = expect;
}
