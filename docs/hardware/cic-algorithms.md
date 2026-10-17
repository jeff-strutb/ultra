# CIC / PIF algorithms: functional specification

This is a clean-room functional specification. It was written by a separate party (the
"spec writer") from the sources cited below, for use by an implementer who has not seen and
will not see those sources. It describes behaviour in prose and mathematics only. No code
from the cited sources is reproduced, paraphrased statement by statement, or mirrored in
structure. The order in which this document lists the updates is the spec writer's own
(grouped by state word), not the order used in any source. The only ordering constraints an
implementation must obey are the data dependencies stated explicitly below.

## Sources and licences

| Tag | File / page | Licence found in the local copy |
|---|---|---|
| [PifChecksum] | `third_party/refsrc/PifChecksum.c` (jago85/PifChecksum) | MIT licence, "Copyright (c) 2019 jago85" (header comment of the file). |
| [UltraCIC] | `third_party/refsrc/cic_c.c` (jago85/UltraCIC_C) | **None found.** The file has no licence or copyright header, and no LICENSE file is present in the local copy. Treat as all rights reserved; only facts (not expression) are taken from it. |
| [n64brew] | `third_party/n64brew/raw/PIF-NUS.wiki`, `CIC-NUS.wiki` | No licence text in the local raw pages. |

Function names cited: [PifChecksum] `ChecksumFunction`, `InitializeChecksum`,
`CalculateChecksum`, `FinalizeChecksum`. [UltraCIC] `Cic6105Algo` (lines 292-322) and
`Cic6105Mode` (lines 376-402).

## Notation

* All values are unsigned 32-bit integers unless stated; `+`, `-`, `*` on them are modulo
  2^32 unless a wider width is stated.
* `xor` is bitwise exclusive or; `[P]` is 1 if predicate P holds, else 0.
* `x < y` on 32-bit values is an **unsigned** comparison everywhere in this document.
* `ROTR(x, r)`: rotate the 32-bit value x right by r bit positions, r in 0..31.
  `ROTL(x, r)`: rotate left by r. Rotation by 0 is the identity. (Note for implementers: on
  the original MIPS hardware the shift instructions use only the low 5 bits of the amount, so
  the "shift by 32 - r" half of a rotate with r = 0 becomes a shift by 0, and OR-ing x with x
  gives x. A host implementation must not perform a 32-bit shift by 32; it must special-case
  r = 0 or use a native rotate.) [PifChecksum] `CalculateChecksum`, `FinalizeChecksum`.
* `lo5(x)` = x mod 32 (bits 4..0). `hi5(x)` = floor(x / 2^27) (bits 31..27, a value 0..31).
* `bit_n(x)` = bit n of x.

## 1. IPL2 checksum

Purpose: IPL2 (running on the VR4300 from RSP IMEM) computes this 48-bit value over IPL3 and
hands it to the PIF, which compares it with the value the CIC supplied at power-on
[n64brew PIF-NUS "IPL2 checksum algorithm", "Console startup"].

### 1.1 Inputs and output

* Input words: W[0..1007], where W[k] is the big-endian 32-bit word at cartridge ROM byte
  offset 0x40 + 4k. That is exactly the 0xFC0 bytes of IPL3, ROM 0x40..0xFFF. No byte of ROM
  0x1000 or later is used. [PifChecksum] `ChecksumFile` (base offset 0x40),
  `CalculateChecksum`.
* Seed: an 8-bit value s (the "IPL2 seed" from the CIC, e.g. 0x3F for 6102). Only its low
  8 bits are used. [PifChecksum] `InitializeChecksum`; seed values from [n64brew PIF-NUS] table.
* Output: a 48-bit unsigned value.
* Constant: `K = 0x6C078965`. [PifChecksum] (the constant it names as a magic number). This
  is the constant in Nintendo's IPL2. Numerically it equals 1812433253, the well-known
  multiplier of the Knuth / MT19937 seeding recurrence. [n64brew PIF-NUS] lists the same
  value as the IPL3 magic for the 6103 and 6106 IPL3s.

### 1.2 The mixing function M

M takes three 32-bit arguments (a, b, c):

1. If b = 0, let b' = c; otherwise b' = b.
2. Let p = a * b' computed as a full 64-bit unsigned product (32 x 32 -> 64, no truncation).
3. Let H = floor(p / 2^32) (upper 32 bits) and L = p mod 2^32 (lower 32 bits).
4. Let d = (H - L) mod 2^32.
5. M(a, b, c) = d if d != 0, and a if d = 0.

The fallback in step 5 returns the **first argument a**, not b or b'.

[PifChecksum] `ChecksumFunction`.

Spot values (computed from the definition above):
M(3, 0, 7) = 0xFFFFFFEB (b replaced by 7, p = 21, d = 0 - 21).
M(0, 5, 1) = 0x00000000 (p = 0, d = 0, so return a = 0).
M(0xFFFFFFFF, 0xFFFFFFFF, 1) = 0xFFFFFFFD (p = 0xFFFFFFFE_00000001).

In every use below, the third argument c is the current step index (i in the main phase, j or
16 in the final phase). It matters only when the second argument is 0.

### 1.3 State and initial value

The state is sixteen 32-bit words S_0..S_15. Let

  I = ((K * s + 1) mod 2^32) xor W[0]

and set every S_n = I for n = 0..15. [PifChecksum] `InitializeChecksum`.

Example: for s = 0x3F, (K * s + 1) mod 2^32 = 0x95DACFDC.

### 1.4 Main phase: steps i = 1, 2, ..., 1008

Step i consumes one input word. For each step define:

* x = W[i - 1] (the current word);
* v = the previous word: v = W[i - 2] for i >= 2, and **v = W[0] for i = 1** (the first step
  treats word 0 as its own predecessor);
* n = W[i] (the following word). It is needed only for i <= 1007. At i = 1008 it would be the
  word at ROM 0x1000, which is never used.

Derived quantities of step i (all from x and v only):

* A = ROTR(x, lo5(v))
* B = ROTL(x, hi5(v))

Update rules. Primed names (S'_m) are the value after step i. Unprimed names are the value
before step i. Each rule except S_6 and S_12 depends only on the word's own old value and the
inputs. S'_6 depends on S'_3 and S'_4, and S'_12 depends on S'_8. Any evaluation order that
respects those dependencies gives the same result.

Words updated at every step i = 1..1008:

* S'_0 = S_0 + M((1007 - i) mod 2^32, x, i).
  At i = 1008 the first argument is 0xFFFFFFFF (the subtraction wraps).
* S'_1 = M(S_1, x, i).
* S'_2 = S_2 xor x.
* S'_3 = S_3 + M(x + 5, K, i). (K is non-zero, so the b = 0 fallback never fires here.)
* S'_4 = S_4 + A.
* S'_5 = S_5 + B.
* S'_6: if x < S_6 (the **old** S_6), then S'_6 = (S'_3 + S_6) xor (x + i);
  otherwise S'_6 = (S'_4 + x) xor S_6. Both branches use the **new** S_3 / S_4 of this step.
* S'_7 = M(S_7, ROTL(x, lo5(v)), i).
* S'_8 = M(S_8, ROTR(x, hi5(v)), i).
* S'_9: if v < x, S'_9 = M(S_9, x, i); otherwise S'_9 = S_9 + x.

Words updated only at steps i = 1..1007 (unchanged by step 1008):

* S'_10 = M(S_10 + x, n, i).
* S'_11 = M(S_11 xor x, n, i).
* S'_12 = S_12 + (S'_8 xor x). This uses the **new** S_8 of this step.
* S'_13 = S_13 + ROTR(x, lo5(x)) + ROTR(n, lo5(n)).
* S'_14 = M( M(S_14, A, i), ROTR(n, lo5(x)), i ).
* S'_15 = M( M(S_15, B, i), ROTL(n, hi5(x)), i ).

Note the rotate amounts. In A, B, S_7 and S_8 the amount comes from the **previous** word v.
In S_13 each value is rotated by its own low 5 bits. In S_14 and S_15 the **next** word n is
rotated by an amount from the **current** word x.

Termination: after step 1008 has applied the ten updates S_0..S_9, the main phase ends.
Altogether S_0..S_9 receive 1008 updates and S_10..S_15 receive 1007. Every word W[0..1007] is
consumed as x exactly once. W[0] is also used as the initial v and in I.

[PifChecksum] `CalculateChecksum`.

### 1.5 Final phase

Initialise four words T_0 = T_1 = T_2 = T_3 = S_0 (the final S_0 from the main phase).

For j = 0, 1, ..., 15 in increasing order, let y = S_j and apply, in this order:

1. T_0 <- T_0 + ROTR(y, lo5(y)).
2. If y < T_0 (using the T_0 just updated in item 1), T_1 <- T_1 + y; otherwise
   T_1 <- M(T_1, y, j).
3. If bit_1(y) = bit_0(y), T_2 <- T_2 + y; otherwise T_2 <- M(T_2, y, j).
4. If bit_0(y) = 1, T_3 <- T_3 xor y; otherwise T_3 <- M(T_3, y, j).

Here j is 0-based, so the first call can be M(., 0, 0). In that case b' = 0, p = 0 and the
result is the first argument.

Then:

* H = M(T_0, T_1, 16)
* L = T_2 xor T_3
* Checksum (48 bits) = (H mod 2^16) * 2^32 + L.

That is, bits 47..32 are the low 16 bits of H, bits 31..0 are L, and the upper 16 bits of H
are discarded. [PifChecksum] `FinalizeChecksum`. The value is 6 bytes wide [n64brew PIF-NUS].

### 1.6 Test vectors

* **Hardware-derived (normative):** the 6102/7101 IPL3 with seed 0x3F gives
  **0xA536C0F1D859** [n64brew PIF-NUS seed/checksum table; CIC-NUS "Checksum" section]. The
  same table gives 6101 -> 0x45CC73EE317A, 7102 -> 0x44160EC5D9AF (seed 0x3F),
  6103/7103 -> 0x586FD4709867 (seed 0x78), 6105/7105 -> 0x8618A45BC2D3 (seed 0x91), and
  6106/7106 -> 0x2BBAD4E6EB74 (seed 0x85), each over its own IPL3.
* **Verified:** an independent implementation written by the spec writer from sections
  1.2-1.5 of this document (not from the source code) reproduces 0xA536C0F1D859. The input
  was the 6102 IPL3 embedded in `tests/roms/krom/CPUTest/CPU/ADD/CPUADD.N64`, bytes
  0x40..0xFFF, SHA-256 `61e88238552c356c23d19409fe5570ee6910419586bc6fc740f638f761adc46e`,
  with W[0] = 0x40806800. For this input and seed, I = 0xD55AA7DC.
* **Derived, not hardware-confirmed** (same 6102 IPL3, other seeds; use these only as
  regression values): seed 0x78 -> 0xD154D024B1BF, seed 0x91 -> 0xF1FA53D0DD55,
  seed 0x85 -> 0x5D6B534D2A7F.

## 2. CIC-NUS-6105 challenge/response transform

### 2.1 Protocol context

When the CIC gets the 2-bit "challenge" command (bits 10), a 6105/7105 does the following
[n64brew CIC-NUS "Command Challenge"; UltraCIC `Cic6105Mode`, lines 376-402]:

1. It sends two nibbles 0xA, 0xA (a wait count for the PIF).
2. It receives 30 nibbles q_0..q_29 in order. Each nibble is received most significant bit
   first.
3. It computes 30 response nibbles r_0..r_29 with the transform below.
4. It sends a single 0 bit, then r_0..r_29 in order, each MSB first.

Non-6105 CICs instead return the bitwise complement of each challenge nibble
[n64brew CIC-NUS].

### 2.2 Transform

The state is a 4-bit accumulator a and a 1-bit carry c, with initial values a = 5 and c = 1.
Process k = 0, 1, ..., 29 in order. With q = q_k:

1. a1 = (a + 8 * [q is even]) mod 16. Even means bit_0(q) = 0.
2. a2 = (a1 + 4 * [bit_1(a1) = 0]) mod 16. Adding 8 never changes bit 1, so this test is
   equivalent to testing bit_1(a).
3. t = (a2 + q) mod 16.
4. u = (2t + 7 * [c = 0]) mod 16.
5. z = u + t + c, computed as an ordinary integer in the range 0..31 (no reduction yet).
6. The new carry is c <- [z >= 16].
7. The response nibble is r_k = 15 - (z mod 16), the 4-bit complement of z mod 16.
8. The new accumulator is a <- r_k. The response nibble feeds the next step.

[UltraCIC] `Cic6105Algo`, lines 292-322.

The transform is a sequential scan. Each r_k depends on q_0..q_k only.

### 2.3 Constant tables and their origin

* This formulation uses **no lookup table**. Its only constants are the initial values a = 5
  and c = 1 and the addends 8, 4, 7 (plus the 4-bit complement). [UltraCIC] `Cic6105Algo`.
* Origin caution: [n64brew CIC-NUS] says the genuine CIC firmware runs this algorithm with
  "some tables in ROM", and that the algorithm was reverse engineered (before the CIC
  firmware was dumped). [UltraCIC] credits Mike Ryan, John McMaster and marshallh, and does
  not say how the constants were obtained. The constants above should therefore be treated as
  **possibly derived from analysis of Nintendo's CIC firmware**. They are functional facts of
  the algorithm, not a copied data table.
* [UltraCIC] also contains 32-nibble NTSC/PAL initial-RAM tables and a "compare mode"
  routine. These are **not** used by the 6105 transform and are out of scope here. They are
  most likely CIC-firmware-derived and must not be imported without separate review.

### 2.4 Test vectors (derived, not hardware-confirmed)

These were computed with the spec writer's implementation of section 2.2. The first three
nibbles of vector (a) were also traced by hand against [UltraCIC] `Cic6105Algo` and match.
Nibbles are written as hex digits, q_0 first.

| | Challenge q_0..q_29 | Response r_0..r_29 |
|---|---|---|
| (a) | `000000000000000000000000000000` | `BF9F9F9F9F9F9F9F9F9F9F9F9F9F9F` |
| (b) | `FFFFFFFFFFFFFFFFFFFFFFFFFFFFFF` | `6949A3250F49A3250F49A3250F49A3` |
| (c) | `0123456789ABCDEF0123456789ABCD` | `B48B98AB7E8338255A29DC41F60B18` |

Hand trace for (a): k=0: a=5, c=1, q=0 -> a1=13, a2=1, t=1, u=2, z=4 -> c=0, r=0xB.
k=1: a=11, c=0 -> a1=3, a2=3, t=3, u=13, z=16 -> c=1, r=0xF.
k=2: a=15, c=1 -> a1=7, a2=7, t=7, u=14, z=22 -> c=1, r=0x9.

## 3. Facts of Nintendo-firmware origin (licensing note)

* The whole IPL2 checksum algorithm (section 1) describes Nintendo's IPL2 code in the PIF
  boot ROM, reverse engineered by [PifChecksum]. Its comments are IPL2 IMEM program-counter
  addresses (0xA400120C..0xA4001510). The constants K = 0x6C078965, 1007/1008, +5, the
  0xFFFF truncation and the step structure are facts about that firmware.
* The expected 48-bit checksums and the seeds (0x3F, 0x78, 0x91, 0x85) are secrets held in
  CIC firmware, as observed on the CIC-PIF bus [n64brew CIC-NUS, PIF-NUS].
* The 6105 constants (section 2.3) may come from analysis of the CIC firmware.
* The IPL3 bytes used for the hardware test vector are Nintendo code (embedded in the krom
  test ROMs). Use them as test input only. Do not copy them into source.
