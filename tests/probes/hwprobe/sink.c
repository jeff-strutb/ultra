// The full record: every series, with no size limit. On a flashcart with an SD card (SC64,
// EverDrive-64 X7, 64drive: libdragon libcart) it is a file per stage on the card; in an
// emulator with an ISViewer port it streams as hex lines that ultra_harness collects. The
// FlashRAM record (record.c) is the compact fallback. Format: docs/timing/hwprobe.md.
#include <libdragon.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "hwp.h"

static FILE* sd_file;
static bool isv;
static int stage;
static uint32_t nbytes, nseries, crc;

#define HWP_DIR "sd:/ultra_hwprobe"

static void isv_hex(const char* tag, const void* data, uint32_t len) {
  static const char hex[] = "0123456789abcdef";
  const uint8_t* p = data;
  char line[16 + 2 * 64 + 2];
  while (len) {
    const uint32_t n = len > 64 ? 64 : len;
    int o = snprintf(line, sizeof line, "HWPX %s ", tag);
    for (uint32_t i = 0; i < n; ++i) {
      line[o++] = hex[p[i] >> 4];
      line[o++] = hex[p[i] & 15];
    }
    line[o++] = '\n';
    line[o] = 0;
    fputs(line, stderr);  // libdragon's debug channel
    p += n;
    len -= n;
  }
}

static void emit(const void* data, uint32_t len) {
  if (sd_file) fwrite(data, 1, len, sd_file);
  if (isv) isv_hex("D", data, len);
  crc = crc32_update(crc, data, len);
  nbytes += len;
}

const char* sink_open(int st, bool sd, bool isviewer) {
  stage = st;
  isv = isviewer;
  nbytes = nseries = crc = 0;
  sd_file = NULL;
  static char path[64];
  if (sd) {
    mkdir(HWP_DIR, 0777);
    snprintf(path, sizeof path, HWP_DIR "/stage%d.bin", st);
    sd_file = fopen(path, "wb");
  }
  uint8_t blank[HWP_HEADER_SIZE];
  memset(blank, 0xFF, sizeof blank);
  if (sd_file) fwrite(blank, 1, sizeof blank, sd_file);
  if (isv) fprintf(stderr, "HWPX BEGIN %d\n", st);
  return sd_file ? path : NULL;
}

bool sink_active(void) { return sd_file || isv; }

void sink_series(uint16_t id, uint8_t esize, uint8_t flags, uint32_t count, const void* data,
                 uint32_t data_len) {
  if (!sink_active()) return;
  uint8_t h[12];
  const uint32_t dcrc = crc32_update(0, data, data_len);
  h[0] = id >> 8;
  h[1] = id;
  h[2] = esize;
  h[3] = flags;
  h[4] = count >> 24;
  h[5] = count >> 16;
  h[6] = count >> 8;
  h[7] = count;
  h[8] = dcrc >> 24;
  h[9] = dcrc >> 16;
  h[10] = dcrc >> 8;
  h[11] = dcrc;
  emit(h, 12);
  emit(data, data_len);
  static const uint8_t zero[4] = {0, 0, 0, 0};
  if (data_len & 3) emit(zero, 4 - (data_len & 3));
  ++nseries;
}

// Header: as the FlashRAM header (record.c), magic ULTRAHF1/2.
bool sink_close(uint32_t status, uint64_t silicon) {
  uint8_t h[HWP_HEADER_SIZE];
  memset(h, 0, sizeof h);
  memcpy(h, stage == 1 ? "ULTRAHF1" : stage == 2 ? "ULTRAHF2" : "ULTRAHF0", 8);
  const uint32_t w[5] = {1, nbytes, crc, nseries, status};
  for (int i = 0; i < 5; ++i)
    for (int b = 0; b < 4; ++b) h[8 + 4 * i + b] = w[i] >> (24 - 8 * b);
  strncpy((char*)h + 28, HWP_BUILD_ID, 16);
  for (int b = 0; b < 8; ++b) h[44 + b] = silicon >> (56 - 8 * b);
  bool ok = true;
  if (sd_file) {
    ok = fseek(sd_file, 0, SEEK_SET) == 0 && fwrite(h, 1, sizeof h, sd_file) == sizeof h;
    ok = fclose(sd_file) == 0 && ok;
    sd_file = NULL;
  }
  if (isv) {
    isv_hex("H", h, sizeof h);
    fprintf(stderr, "HWPX END %d\n", stage);
  }
  return ok;
}

uint32_t sink_bytes(void) { return nbytes; }
