// Record writer: a RAM image of one stage area, programmed into FlashRAM page by page as it
// fills. Format: docs/timing/hwprobe.md.
#include <string.h>

#include "hwp.h"

#ifndef HWP_BUILD_ID
#define HWP_BUILD_ID "unknown"
#endif

#define PAGE 128

static uint8_t img[HWP_STAGE1_SIZE] __attribute__((aligned(16)));
static uint8_t verify[PAGE * 16] __attribute__((aligned(16)));
static uint32_t base;      // FlashRAM offset of this stage's area
static uint32_t size;      // bytes in the area
static uint32_t pos;       // next free byte in img
static uint32_t next_page; // first page of the area (relative) not yet programmed; page 0 is last
static uint32_t nseries;
static uint32_t crc;
static bool overflow;
static bool program_failed;
static int stage;
static uint64_t silicon;

static uint32_t crc_table[256];

uint32_t crc32_update(uint32_t c, const void* data, uint32_t len) {
  if (!crc_table[1]) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t r = i;
      for (int k = 0; k < 8; ++k) r = (r >> 1) ^ (r & 1 ? 0xEDB88320u : 0);
      crc_table[i] = r;
    }
  }
  const uint8_t* p = data;
  c = ~c;
  while (len--) c = crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
  return ~c;
}

static void put32(uint8_t* p, uint32_t v) {
  p[0] = v >> 24;
  p[1] = v >> 16;
  p[2] = v >> 8;
  p[3] = v;
}

void rec_begin(int st, uint64_t silicon_id) {
  stage = st;
  silicon = silicon_id;
  base = st == 1 ? HWP_STAGE1_BASE : HWP_STAGE2_BASE;
  size = st == 1 ? HWP_STAGE1_SIZE : HWP_STAGE2_SIZE;
  memset(img, 0xFF, sizeof img);
  pos = HWP_HEADER_SIZE;
  next_page = 1;
  nseries = 0;
  crc = 0;
  overflow = false;
  program_failed = false;
}

uint32_t rec_bytes(void) { return pos - HWP_HEADER_SIZE; }
uint32_t rec_crc(void) { return crc; }
uint32_t rec_series_count(void) { return nseries; }
uint32_t rec_room(void) { return size - pos; }
const uint8_t* rec_image(void) { return img; }
uint64_t rec_silicon(void) { return silicon; }

static void append(const void* data, uint32_t len) {
  memcpy(img + pos, data, len);
  crc = crc32_update(crc, data, len);
  pos += len;
}

static bool reserve(uint32_t data_len) {
  const uint32_t need = 12 + ((data_len + 3) & ~3u);
  if (pos + need > size) {
    overflow = true;
    return false;
  }
  return true;
}

static void series_header(uint16_t id, uint8_t esize, uint8_t flags, uint32_t count,
                          const void* data, uint32_t data_len) {
  uint8_t h[12];
  h[0] = id >> 8;
  h[1] = id;
  h[2] = esize;
  h[3] = flags;
  put32(h + 4, count);
  put32(h + 8, crc32_update(0, data, data_len));
  append(h, 12);
}

static void pad4(void) {
  static const uint8_t zero[4] = {0, 0, 0, 0};
  if (pos & 3) append(zero, 4 - (pos & 3));
}

void rec_series(uint16_t id, uint8_t esize, uint32_t count, const void* data) {
  const uint32_t len = esize * count;
  sink_series(id, esize, HWP_PACK_NONE, count, data, len);
  if (!reserve(len)) return;
  series_header(id, esize, HWP_PACK_NONE, count, data, len);
  append(data, len);
  pad4();
  ++nseries;
}

// Packed byte series (series.h HWP_PACK_*): u8 first, u8 base, u16 escapes, nibbles (high
// first) of item - base, 15 meaning "next escape byte", then the escape bytes. DELTAS packs the
// n - 1 deltas mod 256 of the sequence, VALUES packs all n values with first = 0.
static uint8_t pack_buf[HWP_RAW_SAMPLES_VI + 64];
static uint8_t esc_buf[HWP_RAW_SAMPLES_VI];

void rec_series_packed(uint16_t id, uint8_t mode, uint32_t count, const uint8_t* data) {
  if (count == 0 || count > HWP_RAW_SAMPLES_VI) return;
  const uint32_t first_item = mode == HWP_PACK_DELTAS ? 1 : 0;
  uint8_t lo = 0xFF;
  for (uint32_t i = first_item; i < count; ++i) {
    const uint8_t v = mode == HWP_PACK_DELTAS ? (uint8_t)(data[i] - data[i - 1]) : data[i];
    if (v < lo) lo = v;
  }
  if (first_item == count) lo = 0;
  uint32_t nesc = 0;
  uint32_t nib = 0;
  uint8_t* out = pack_buf + 4;
  memset(out, 0, sizeof pack_buf - 4);
  for (uint32_t i = first_item; i < count; ++i) {
    const uint8_t v = mode == HWP_PACK_DELTAS ? (uint8_t)(data[i] - data[i - 1]) : data[i];
    uint8_t q = v - lo;
    if (v - lo >= 15) {
      q = 15;
      esc_buf[nesc++] = v;
    }
    out[nib >> 1] |= (nib & 1) ? q : (uint8_t)(q << 4);
    ++nib;
  }
  const uint32_t nib_bytes = (nib + 1) / 2;
  pack_buf[0] = mode == HWP_PACK_DELTAS ? data[0] : 0;
  pack_buf[1] = lo;
  pack_buf[2] = nesc >> 8;
  pack_buf[3] = nesc;
  memcpy(out + nib_bytes, esc_buf, nesc);
  const uint32_t len = 4 + nib_bytes + nesc;
  sink_series(id, 1, mode, count, pack_buf, len);
  if (!reserve(len)) return;
  series_header(id, 1, mode, count, pack_buf, len);
  append(pack_buf, len);
  pad4();
  ++nseries;
}

// Pages are programmed whatever silicon ID the cart reported: an emulated chip may report its
// own. Success is decided by reading the record back (rec_finish).
// Large series go to the full record only (sink.c), never to FlashRAM.
void rec_series_full(uint16_t id, uint8_t esize, uint32_t count, const void* data) {
  sink_series(id, esize, HWP_PACK_NONE, count, data, esize * count);
}

void rec_flush(void) {
  while ((next_page + 1) * PAGE <= pos) {
    if (!flash_program_page(base / PAGE + next_page, img + next_page * PAGE))
      program_failed = true;
    ++next_page;
  }
}

bool rec_program_errors(void) { return program_failed; }

bool rec_finish(uint32_t status) {
  // Tail page(s), then the header page.
  while (next_page * PAGE < pos) {
    if (!flash_program_page(base / PAGE + next_page, img + next_page * PAGE))
      program_failed = true;
    ++next_page;
  }
  uint8_t* h = img;
  memcpy(h, stage == 1 ? "ULTRAHW1" : "ULTRAHW2", 8);
  put32(h + 8, 1);
  put32(h + 12, rec_bytes());
  put32(h + 16, crc);
  put32(h + 20, nseries);
  put32(h + 24, status ? status : (overflow ? 0xFFFF0001u : 0));
  memset(h + 28, 0, 16);
  strncpy((char*)h + 28, HWP_BUILD_ID, 16);
  put32(h + 44, (uint32_t)(silicon >> 32));
  put32(h + 48, (uint32_t)silicon);
  memset(h + 52, 0, 12);
  if (!flash_program_page(base / PAGE, img)) program_failed = true;

  // Read back and compare.
  bool same = true;
  for (uint32_t off = 0; off < next_page * PAGE; off += sizeof verify) {
    uint32_t n = next_page * PAGE - off;
    if (n > sizeof verify) n = sizeof verify;
    flash_read(base + off, verify, n);
    if (memcmp(verify, img + off, n) != 0) same = false;
  }
  return same && !overflow;
}
