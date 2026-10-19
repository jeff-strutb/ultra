// Peripherals: every Joybus command the ROM may send, on the four controller ports and the
// cartridge port, through raw SI DMA to PIF RAM; accessory identification; PIF RAM frame edge
// cases; and the AI DMA FIFO. docs/timing/hwprobe.md, "Peripherals".
//
// Nothing here writes a Controller Pak, an EEPROM or the RTC. The only Joybus commands sent are
// info, read buttons, reset, read accessory, EEPROM read and RTC info/read, plus unknown command
// bytes chosen away from every documented write command (0x03 accessory write, 0x05 EEPROM
// write, 0x08 RTC write, 0x0A-0x0D voice, 0x13 keyboard, 0x15 link cable write). The PIF control
// byte is only ever 0x00 or 0x01. Every SI and AI wait is bounded.
#include <string.h>

#include "hwp.h"

// Count advances at half the 93.75 MHz PCycle rate (n64brew "ROM Header", Clock Rate).
#define TICKS_PER_MS 46875u
#define SI_TIMEOUT (10u * TICKS_PER_MS)

// ---- SI and PIF RAM (n64brew "Serial Interface", "PIF-NUS") ----
#define SI_REG(i) (((volatile uint32_t*)0xA4800000u)[i])
enum { SIR_DRAM_ADDR = 0, SIR_PIF_AD_RD64B = 1, SIR_PIF_AD_WR64B = 4, SIR_STATUS = 6 };
#define SI_STATUS_DMA_BUSY 0x1u
#define SI_STATUS_IO_BUSY  0x2u
#define PIF_RAM_PHYS    0x1FC007C0u // the last 64 bytes of the PIF's space
#define PIF_RAM_SIZE    64
#define PIF_CTRL_BYTE   63
#define PIF_CTRL_JOYBUS 0x01 // run the frame on the next 64-byte read; no other bit is ever set
// Frame bytes of the Joybus channel walk (n64brew "PIF-NUS", Joybus).
#define PIF_SKIP  0x00
#define PIF_RESET 0xFD
#define PIF_END   0xFE
#define PIF_PAD   0xFF
#define PIF_CHANNELS 5 // four controller ports, then the cartridge port
#define PIF_CART_CHANNEL 4

// ---- Joybus commands and their lengths (n64brew "Joybus Protocol") ----
#define JOY_INFO         0x00 // tx 1, rx 3: device id (2), status
#define JOY_BUTTONS      0x01 // tx 1, rx 4: buttons (2), stick X, stick Y
#define JOY_ACC_READ     0x02 // tx 3 (address with check bits), rx 33: 32 data bytes, data CRC
#define JOY_EEPROM_READ  0x04 // tx 2 (block), rx 8
#define JOY_RTC_INFO     0x06 // tx 1, rx 3
#define JOY_RTC_READ     0x07 // tx 2 (block), rx 9: 8 data bytes, status
#define JOY_RESET        0xFF // tx 1, rx 3: as JOY_INFO
#define JOY_ACC_DATA     32
#define JOY_UNKNOWN_RX   4

// Accessory address check bits: address bits 15:5, times x^5, modulo x^5 + x^4 + x^2 + 1, in
// bits 4:0 (n64brew "Joybus Protocol", Read Accessory; "Controller Pak").
#define ACC_ADDR_MASK     0xFFE0u
#define ACC_ADDR_CRC_POLY 0x35u

// Accessory addresses read on every port. Controller Pak: 0x0000-0x00FF is page 0, the ID and
// label area (n64brew "Controller Pak"). Rumble Pak: 0x8000 identification, 0xC000 motor.
// Transfer Pak: 0x8000 power, 0xA000 bank, 0xB000 status, 0xC000-0xFFFF the Game Boy window
// (n64brew "Rumble Pak", "Transfer Pak"). Reads only: identification by writing 0x80 to 0x8000
// is left out, because on a Controller Pak that address may select a bank.
static const uint16_t acc_addrs[] = {0x0000, 0x0020, 0x0040, 0x0060, 0x0080, 0x00A0, 0x00C0,
                                     0x00E0, 0x8000, 0xA000, 0xB000, 0xC000, 0xE000};
#define ACC_BAD_CRC_ADDR 0x0000u // also read once with its check bits inverted in bit 0

static const uint8_t unknown_port_cmds[] = {0x04, 0x06, 0x07, 0x09, 0x10, 0x14,
                                            0x20, 0x41, 0x7F, 0x80, 0xA5, 0xFE};
static const uint8_t unknown_cart_cmds[] = {0x01, 0x09, 0x7F};
#define EEPROM_BLOCKS_READ 2
#define RTC_BLOCKS_READ    3

// ---- AI (n64brew "Audio Interface") and MI (n64brew "MIPS Interface") ----
#define AI_REG(i) (((volatile uint32_t*)0xA4500000u)[i])
enum { AIR_DRAM_ADDR = 0, AIR_LEN = 1, AIR_CONTROL = 2, AIR_STATUS = 3, AIR_DACRATE = 4,
       AIR_BITRATE = 5 };
#define AI_CONTROL_DMA_ENABLE 0x1u
#define AI_STATUS_FULL 0x80000001u
#define AI_STATUS_BUSY 0x40000000u
#define MI_INTR_REG (*(volatile uint32_t*)0xA4300008u)
#define MI_INTR_AI 0x04u
#define VI_CLOCK_NTSC 48681812u // Hz
#define AI_RATE 44100u
#define AI_BITRATE_VALUE 15u
#define AI_BUF_BYTES 2048u
#define AI_EVENTS_MAX 1024u
#define AI_SUMMARY_EVENTS 12u
#define AI_DRAIN_TIMEOUT (100u * TICKS_PER_MS)
#define AI_RUN_TIMEOUT (80u * TICKS_PER_MS)
#define AI_IDLE_END (1u * TICKS_PER_MS)  // idle this long after the last buffer ends a run
#define AI_UNDERRUN_GAP (2u * TICKS_PER_MS)
#define AI_SETUP_WORDS 8u

#define TIMING_REPS 16

// ---------------------------------------------------------------------------------------------
static uint64_t pif_buf[PIF_RAM_SIZE / 8] __attribute__((aligned(16)));

static bool si_wait_idle(void) {
  const uint32_t t0 = c0_count();
  while (SI_REG(SIR_STATUS) & (SI_STATUS_DMA_BUSY | SI_STATUS_IO_BUSY))
    if (c0_count() - t0 > SI_TIMEOUT) return false;
  return true;
}

// One 64-byte DMA between pif_buf and PIF RAM. Returns its Count ticks (HWP_TIMEOUT if the SI
// did not go idle) and stores SI_STATUS as it was when the DMA ended, before clearing the SI
// interrupt.
static uint32_t si_dma(bool to_pif, uint32_t* status) {
  *status = HWP_TIMEOUT;
  if (!si_wait_idle()) return HWP_TIMEOUT;
  if (to_pif)
    dcache_wb(pif_buf, PIF_RAM_SIZE);
  else
    dcache_inv(pif_buf, PIF_RAM_SIZE);
  SI_REG(SIR_DRAM_ADDR) = PHYS(pif_buf);
  const uint32_t t0 = c0_count();
  SI_REG(to_pif ? SIR_PIF_AD_WR64B : SIR_PIF_AD_RD64B) = PIF_RAM_PHYS;
  uint32_t t = HWP_TIMEOUT;
  while (c0_count() - t0 <= SI_TIMEOUT) {
    if (!(SI_REG(SIR_STATUS) & (SI_STATUS_DMA_BUSY | SI_STATUS_IO_BUSY))) {
      t = c0_count() - t0;
      break;
    }
  }
  *status = SI_REG(SIR_STATUS);
  SI_REG(SIR_STATUS) = 0;  // any write clears the SI interrupt
  if (!to_pif) dcache_inv(pif_buf, PIF_RAM_SIZE);
  return t;
}

// Writes frame to PIF RAM and reads it back `reads` times. t: write ticks, then each read's
// ticks, then SI_STATUS after the write and after each read. back: `reads` x 64 bytes.
static void pif_run(const uint8_t frame[PIF_RAM_SIZE], int reads, uint8_t* back, uint32_t* t) {
  memcpy(pif_buf, frame, PIF_RAM_SIZE);
  t[0] = si_dma(true, &t[1 + reads]);
  for (int r = 0; r < reads; ++r) {
    t[1 + r] = si_dma(false, &t[2 + reads + r]);
    memcpy(back + PIF_RAM_SIZE * r, pif_buf, PIF_RAM_SIZE);
  }
}

static uint16_t acc_addr(uint16_t a) {
  uint32_t r = a & ACC_ADDR_MASK;
  for (int b = 15; b >= 5; --b)
    if (r & (1u << b)) r ^= ACC_ADDR_CRC_POLY << (b - 5);
  return (uint16_t)((a & ACC_ADDR_MASK) | (r & 0x1Fu));
}

static uint32_t be32(const uint8_t* p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

// ---- Command cases ----
typedef struct {
  uint8_t ch, cmd, tx, rx;
  uint8_t arg[2];  // the tx bytes after the command (tx - 1 of them)
} joy_case_t;

#define MAX_CASES 160
static joy_case_t cases[MAX_CASES];
static int ncases;

static void add_case(uint8_t ch, uint8_t cmd, uint8_t tx, uint8_t rx, uint8_t a0, uint8_t a1) {
  if (ncases < MAX_CASES) cases[ncases++] = (joy_case_t){ch, cmd, tx, rx, {a0, a1}};
}

static void build_cases(void) {
  ncases = 0;
  for (uint8_t ch = 0; ch < PIF_CART_CHANNEL; ++ch) {
    add_case(ch, JOY_INFO, 1, 3, 0, 0);
    add_case(ch, JOY_BUTTONS, 1, 4, 0, 0);
    for (unsigned i = 0; i < sizeof acc_addrs / sizeof acc_addrs[0]; ++i) {
      const uint16_t a = acc_addr(acc_addrs[i]);
      add_case(ch, JOY_ACC_READ, 3, JOY_ACC_DATA + 1, a >> 8, a & 0xFF);
    }
    const uint16_t bad = acc_addr(ACC_BAD_CRC_ADDR) ^ 1u;
    add_case(ch, JOY_ACC_READ, 3, JOY_ACC_DATA + 1, bad >> 8, bad & 0xFF);
    add_case(ch, JOY_INFO, 1, 3, 0, 0);  // status after the bad check bits
    for (unsigned i = 0; i < sizeof unknown_port_cmds; ++i)
      add_case(ch, unknown_port_cmds[i], 1, JOY_UNKNOWN_RX, 0, 0);
    add_case(ch, JOY_RESET, 1, 3, 0, 0);
    add_case(ch, JOY_INFO, 1, 3, 0, 0);
  }
  const uint8_t c = PIF_CART_CHANNEL;
  add_case(c, JOY_INFO, 1, 3, 0, 0);
  add_case(c, JOY_RESET, 1, 3, 0, 0);
  for (uint8_t b = 0; b < EEPROM_BLOCKS_READ; ++b) add_case(c, JOY_EEPROM_READ, 2, 8, b, 0);
  add_case(c, JOY_RTC_INFO, 1, 3, 0, 0);
  for (uint8_t b = 0; b < RTC_BLOCKS_READ; ++b) add_case(c, JOY_RTC_READ, 2, 9, b, 0);
  for (unsigned i = 0; i < sizeof unknown_cart_cmds; ++i)
    add_case(c, unknown_cart_cmds[i], 1, JOY_UNKNOWN_RX, 0, 0);
}

// One command on its channel: skips, tx, rx, command, arguments, rx bytes preset to 0xFF, end.
// Returns the offset of the rx length byte.
static int build_frame(const joy_case_t* c, uint8_t f[PIF_RAM_SIZE]) {
  memset(f, 0, PIF_RAM_SIZE);
  int i = 0;
  for (int k = 0; k < c->ch; ++k) f[i++] = PIF_SKIP;
  f[i++] = c->tx;
  const int rx_at = i;
  f[i++] = c->rx;
  f[i++] = c->cmd;
  for (int k = 0; k + 1 < c->tx; ++k) f[i++] = c->arg[k];
  memset(f + i, PIF_PAD, c->rx);
  i += c->rx;
  f[i] = PIF_END;
  f[PIF_CTRL_BYTE] = PIF_CTRL_JOYBUS;
  return rx_at;
}

#define CMD_FULL_WORDS (2 * PIF_RAM_SIZE / 4 + 4)
#define CMD_SUMMARY_WORDS 3
static uint32_t cmd_full[MAX_CASES * CMD_FULL_WORDS];
static uint32_t cmd_summary[MAX_CASES * CMD_SUMMARY_WORDS];
static uint32_t devices[PIF_CHANNELS * 3];

static void run_cases(void) {
  static uint8_t frame[PIF_RAM_SIZE], back[PIF_RAM_SIZE];
  bool have_info[PIF_CHANNELS] = {false}, have_buttons[PIF_CHANNELS] = {false};
  for (int n = 0; n < ncases; ++n) {
    const joy_case_t* c = &cases[n];
    const int rx_at = build_frame(c, frame);
    uint32_t t[4];
    pif_run(frame, 1, back, t);
    uint32_t* w = &cmd_full[n * CMD_FULL_WORDS];
    memcpy(w, frame, PIF_RAM_SIZE);
    memcpy(w + PIF_RAM_SIZE / 4, back, PIF_RAM_SIZE);
    w[32] = t[0];  // write ticks
    w[33] = t[1];  // read ticks
    w[34] = t[2];  // SI_STATUS after the write
    w[35] = t[3];  // SI_STATUS after the read
    const uint8_t* r = back + rx_at + 1 + c->tx;  // first response byte
    uint32_t* s = &cmd_summary[n * CMD_SUMMARY_WORDS];
    s[0] = (uint32_t)c->ch << 24 | (uint32_t)c->cmd << 16 | (uint32_t)c->arg[0] << 8 | c->arg[1];
    s[1] = (uint32_t)back[rx_at] << 24 | (uint32_t)r[0] << 16 | (uint32_t)r[1] << 8 | r[2];
    s[2] = t[1];
    uint32_t* d = &devices[c->ch * 3];
    if (c->cmd == JOY_INFO && !have_info[c->ch]) {
      have_info[c->ch] = true;
      d[0] = s[1];
    }
    if (c->cmd == JOY_BUTTONS && !have_buttons[c->ch]) {
      have_buttons[c->ch] = true;
      d[1] = back[rx_at];
      d[2] = be32(r);
    }
  }
}

// Repeated info and buttons commands on each channel, for the spread of the SI times.
static uint32_t timing[PIF_CHANNELS * 2 * TIMING_REPS * 2];

static void run_timing(void) {
  static uint8_t frame[PIF_RAM_SIZE], back[PIF_RAM_SIZE];
  uint32_t n = 0;
  for (uint8_t ch = 0; ch < PIF_CHANNELS; ++ch) {
    for (int k = 0; k < 2; ++k) {
      const joy_case_t c = {ch, k ? JOY_BUTTONS : JOY_INFO, 1, k ? 4 : 3, {0, 0}};
      build_frame(&c, frame);
      for (int rep = 0; rep < TIMING_REPS; ++rep) {
        uint32_t t[4];
        pif_run(frame, 1, back, t);
        timing[n++] = t[0];
        timing[n++] = t[1];
      }
    }
  }
}

// ---- PIF RAM frame edge cases ----
// Each frame: `b` from byte 0, `fill` up to byte 62, `ctrl` (0 or PIF_CTRL_JOYBUS) at byte 63.
typedef struct {
  uint8_t ctrl, fill, len;
  uint8_t b[40];
} edge_t;

#define ST 0x01, 0x03, JOY_INFO, 0xFF, 0xFF, 0xFF  // info on the current channel
#define Z8 0, 0, 0, 0, 0, 0, 0, 0
#define F8 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
static const edge_t edges[] = {
    {0x00, 0, 7, {ST, PIF_END}},                                    // 0 control byte 0
    {0x01, 0, 10, {PIF_SKIP, PIF_SKIP, PIF_SKIP, ST, PIF_END}},     // 1 three skips: channel 3
    {0x01, 0, 12, {PIF_PAD, PIF_PAD, PIF_PAD, PIF_PAD, PIF_PAD, ST, PIF_END}}, // 2 five pads
    {0x01, 0, 8, {PIF_RESET, ST, PIF_END}},                         // 3 0xFD first
    {0x01, 0, 14, {ST, PIF_RESET, ST, PIF_END}},                    // 4 0xFD between commands
    {0x01, 0, 14, {ST, PIF_END, ST, PIF_END}},                      // 5 command after 0xFE
    {0x01, 0, 14, {ST, PIF_SKIP, ST, PIF_END}},                     // 6 skip between: channel 2
    {0x01, 0x00, 24, {ST, ST, ST, ST}},                             // 7 no 0xFE, 0x00 after
    {0x01, 0xFF, 24, {ST, ST, ST, ST}},                             // 8 no 0xFE, 0xFF after
    {0x01, 0, 37, {ST, ST, ST, ST, ST, ST, PIF_END}},               // 9 six commands
    {0x01, 0, 7, {0x81, 0x03, JOY_INFO, 0xFF, 0xFF, 0xFF, PIF_END}}, // 10 tx bit 7
    {0x01, 0, 7, {0x41, 0x03, JOY_INFO, 0xFF, 0xFF, 0xFF, PIF_END}}, // 11 tx bit 6
    {0x01, 0, 7, {0xC1, 0x03, JOY_INFO, 0xFF, 0xFF, 0xFF, PIF_END}}, // 12 tx bits 7 and 6
    {0x01, 0, 6, {0x40, 0x03, 0xFF, 0xFF, 0xFF, PIF_END}},          // 13 tx length 0 (0x40)
    {0x01, 0, 7, {0x01, 0x83, JOY_INFO, 0xFF, 0xFF, 0xFF, PIF_END}}, // 14 rx bit 7 preset
    {0x01, 0, 7, {0x01, 0x43, JOY_INFO, 0xFF, 0xFF, 0xFF, PIF_END}}, // 15 rx bit 6 preset
    {0x01, 0, 5, {0x01, 0x01, JOY_INFO, 0xFF, PIF_END}},            // 16 rx 1: short
    {0x01, 0, 12, {0x01, 0x08, JOY_INFO, F8, PIF_END}},             // 17 rx 8: long
    {0x01, 0, 4, {0x01, 0x00, JOY_INFO, PIF_END}},                  // 18 rx 0: send only
    {0x01, 0, 9, {0x03, 0x03, JOY_INFO, 0, 0, 0xFF, 0xFF, 0xFF, PIF_END}}, // 19 tx 3
    {0x01, 0, 38, {0x20, 0x03, JOY_INFO, Z8, Z8, Z8, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF,
                   PIF_END}},                                       // 20 tx 32: oversize
    {0x01, 0, 36, {0x01, 0x20, JOY_INFO, F8, F8, F8, F8, PIF_END}}, // 21 rx 32: oversize
    {0x01, 0xFF, 0, {0}},                                           // 22 all 0xFF
    {0x01, 0x00, 0, {0}},                                           // 23 all 0x00
    {0x00, 0x00, 0, {0}},                                           // 24 all 0x00, control 0
    {0x01, 0, 25, {ST, ST, ST, ST, PIF_END}},                       // 25 normal: leave it sane
};
#undef ST
#define NEDGES (sizeof edges / sizeof edges[0])
#undef Z8
#undef F8

#define EDGE_READS 2
#define EDGE_TIME_WORDS (2 + 2 * EDGE_READS) // as pif_run's t
#define EDGE_FULL_WORDS ((1 + EDGE_READS) * PIF_RAM_SIZE / 4 + EDGE_TIME_WORDS)
#define EDGE_SUMMARY_BYTES 40
static uint32_t edge_full[NEDGES * EDGE_FULL_WORDS];
static uint8_t edge_summary[NEDGES * EDGE_SUMMARY_BYTES] __attribute__((aligned(4)));

static void run_edges(void) {
  static uint8_t frame[PIF_RAM_SIZE], back[EDGE_READS * PIF_RAM_SIZE];
  for (unsigned n = 0; n < NEDGES; ++n) {
    const edge_t* e = &edges[n];
    memset(frame, e->fill, PIF_RAM_SIZE);
    memcpy(frame, e->b, e->len);
    frame[PIF_CTRL_BYTE] = e->ctrl & PIF_CTRL_JOYBUS;
    uint32_t t[EDGE_TIME_WORDS];
    pif_run(frame, EDGE_READS, back, t);
    uint32_t* w = &edge_full[n * EDGE_FULL_WORDS];
    memcpy(w, frame, PIF_RAM_SIZE);
    memcpy(w + PIF_RAM_SIZE / 4, back, EDGE_READS * PIF_RAM_SIZE);
    memcpy(w + (1 + EDGE_READS) * PIF_RAM_SIZE / 4, t, sizeof t);
    memcpy(&edge_summary[n * EDGE_SUMMARY_BYTES], back, EDGE_SUMMARY_BYTES);
  }
}

// ---- AI FIFO ----
enum { AI_BOTH = 0, AI_MID = 1, AI_UNDERRUN = 2, AI_NSCEN = 3 };
static uint32_t ai_silence[AI_BUF_BYTES / 4] __attribute__((aligned(8)));
static uint32_t ai_ev[4 * AI_EVENTS_MAX];

static void ai_queue(uint32_t* status, uint32_t* len) {
  AI_REG(AIR_DRAM_ADDR) = PHYS(ai_silence);
  AI_REG(AIR_LEN) = AI_BUF_BYTES;
  *status = AI_REG(AIR_STATUS);
  *len = AI_REG(AIR_LEN);
}

// setup: AI_STATUS before; AI_STATUS, AI_LEN after queueing A; after B; after C (AI_BOTH only);
// all relative to the Count at A's AI_LEN write. Returns the number of events logged.
static uint32_t ai_run(int scen, uint32_t setup[AI_SETUP_WORDS + 1]) {
  for (uint32_t i = 0; i < AI_SETUP_WORDS + 1; ++i) setup[i] = 0;
  setup[0] = AI_REG(AIR_STATUS);
  const uint32_t t0 = c0_count();
  ai_queue(&setup[1], &setup[2]);
  bool pending = scen != AI_BOTH;
  if (!pending) {
    ai_queue(&setup[3], &setup[4]);
    ai_queue(&setup[5], &setup[6]);  // a third buffer while both slots are full
  }
  uint32_t n = 0, prev_len = ~0u, prev_st = ~0u, prev_mi = ~0u, idle_since = 0;
  bool idle = false;
  for (;;) {
    const uint32_t t = c0_count() - t0;
    if (t > AI_RUN_TIMEOUT) break;
    const uint32_t len = AI_REG(AIR_LEN), st = AI_REG(AIR_STATUS), mi = MI_INTR_REG & MI_INTR_AI;
    if (mi) AI_REG(AIR_STATUS) = 0;  // any write clears the AI interrupt
    if ((len != prev_len || st != prev_st || mi != prev_mi) && n < AI_EVENTS_MAX) {
      uint32_t* e = &ai_ev[4 * n++];
      e[0] = t;
      e[1] = len;
      e[2] = st;
      e[3] = mi;
      prev_len = len;
      prev_st = st;
      prev_mi = mi;
    }
    const bool now_idle = !(st & (AI_STATUS_BUSY | AI_STATUS_FULL));
    if (now_idle && !idle) idle_since = t;
    idle = now_idle;
    if (pending) {
      const bool go = scen == AI_MID ? len < AI_BUF_BYTES / 2
                                     : idle && t - idle_since > AI_UNDERRUN_GAP;
      if (go) {
        ai_queue(&setup[3], &setup[4]);
        setup[7] = c0_count() - t0;
        pending = false;
        idle = false;
      }
    } else if (idle && t - idle_since > AI_IDLE_END) {
      break;
    }
  }
  setup[8] = n;
  return n;
}

static void run_ai(void) {
  for (uint32_t i = 0; i < AI_BUF_BYTES / 4; ++i) ai_silence[i] = 0;
  dcache_wb(ai_silence, sizeof ai_silence);
  // DACRATE = VI clock / rate - 1 (n64brew "Audio Interface").
  const uint32_t dacrate = VI_CLOCK_NTSC / AI_RATE - 1;
  uint32_t head[6];
  head[0] = AI_REG(AIR_STATUS);
  AI_REG(AIR_DACRATE) = dacrate;
  AI_REG(AIR_BITRATE) = AI_BITRATE_VALUE;
  AI_REG(AIR_CONTROL) = AI_CONTROL_DMA_ENABLE;
  // Let whatever an earlier group queued play out.
  const uint32_t d0 = c0_count();
  while ((AI_REG(AIR_STATUS) & (AI_STATUS_BUSY | AI_STATUS_FULL)) &&
         c0_count() - d0 < AI_DRAIN_TIMEOUT) {}
  head[1] = AI_REG(AIR_STATUS);
  head[2] = c0_count() - d0;
  head[3] = dacrate;
  head[4] = AI_BITRATE_VALUE;
  head[5] = AI_BUF_BYTES;
  AI_REG(AIR_STATUS) = 0;
  rec_series(S_AI_FIFO_CONFIG, 4, 6, head);

  static const uint16_t full_ids[AI_NSCEN] = {S_AI_FIFO_BOTH, S_AI_FIFO_MID, S_AI_FIFO_UNDERRUN};
  static uint32_t setup[AI_NSCEN * (AI_SETUP_WORDS + 1)];
  static uint32_t summary[AI_NSCEN * AI_SUMMARY_EVENTS * 4];
  memset(summary, 0, sizeof summary);
  for (int s = 0; s < AI_NSCEN; ++s) {
    const uint32_t n = ai_run(s, &setup[s * (AI_SETUP_WORDS + 1)]);
    rec_series_full(full_ids[s], 4, 4 * n, ai_ev);
    // Summary: the first events whose AI_STATUS or interrupt bit changed.
    uint32_t k = 0;
    for (uint32_t i = 0; i < n && k < AI_SUMMARY_EVENTS; ++i) {
      const uint32_t* e = &ai_ev[4 * i];
      const uint32_t* p = i ? e - 4 : e;  // the previous event
      if (i == 0 || e[2] != p[2] || e[3] != p[3]) {
        memcpy(&summary[(s * AI_SUMMARY_EVENTS + k) * 4], e, 16);
        ++k;
      }
    }
  }
  AI_REG(AIR_CONTROL) = 0;
  AI_REG(AIR_STATUS) = 0;
  rec_series(S_AI_FIFO_SETUP, 4, AI_NSCEN * (AI_SETUP_WORDS + 1), setup);
  rec_series(S_AI_FIFO_SUMMARY, 4, AI_NSCEN * AI_SUMMARY_EVENTS * 4, summary);
}

void grp_peripherals(void* unused) {
  (void)unused;
  build_cases();
  run_cases();
  rec_series(S_PER_DEVICES, 4, PIF_CHANNELS * 3, devices);
  rec_series(S_PER_CMD_SUMMARY, 4, ncases * CMD_SUMMARY_WORDS, cmd_summary);
  rec_series_full(S_PER_CMD, 4, ncases * CMD_FULL_WORDS, cmd_full);
  rec_flush();
  run_timing();
  rec_series_full(S_PER_TIMING, 4, sizeof timing / 4, timing);
  run_ai();
  rec_flush();
  // Last: frames the PIF may handle badly. The final edge case is a normal frame.
  run_edges();
  rec_series(S_PER_EDGE_SUMMARY, 1, sizeof edge_summary, edge_summary);
  rec_series_full(S_PER_EDGE, 4, sizeof edge_full / 4, edge_full);
}
