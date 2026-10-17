#pragma once
// Physical bus seen by the VR4300 over SysAD: address decode of the full 32-bit physical map,
// size semantics of each region, and the completion time of every access.
// Source for the map and per-region access-size behaviour: n64brew "Memory map", sections
// "Physical Memory Map" and "Physical Memory Map accesses". docs/hardware/bus.md.
//
// Every access returns its completion tick; nothing on the bus is instantaneous. An access the
// RCP never answers (unmapped regions, cached access outside RDRAM, 64-bit reads of RCP
// registers) returns kTickNever: the CPU is then frozen, exactly as on hardware.

#include "common/types.hpp"
#include "core/clock.hpp"
#include "core/trace.hpp"

namespace reality {

class Scheduler;
class Mi;
class Ri;
class Pi;
class Si;
class SpInterface;
class DpInterface;
class Vi;
class Ai;

struct BusDevices {
  Scheduler* sched;
  Mi* mi;
  Ri* ri;
  Pi* pi;
  Si* si;
  SpInterface* sp;
  DpInterface* dp;
  Vi* vi;
  Ai* ai;
  TraceSink* trace;
};

class Bus {
 public:
  explicit Bus(const BusDevices& d) : d_(d) {}

  // size: 1..4 (single word transfer; data is the aligned 32-bit word in bus lanes),
  // 8 (doubleword), 16 or 32 (cache block). `cached` marks block refills and write-backs.
  // Returns the completion tick or kTickNever if the RCP never answers.
  Tick cpu_read(Master m, u32 paddr, u32 size, u8* out, Tick t);
  Tick cpu_write(Master m, u32 paddr, u32 size, const u8* data, Tick t);

  const BusDevices& devices() const noexcept { return d_; }

 private:
  Tick rcp_read(u32 paddr, u32 size, u8* out, Tick t);
  Tick rcp_write(u32 paddr, u32 size, const u8* data, Tick t);
  void trace_io(Master m, TraceKind k, u32 paddr, const u8* data, u32 len, Tick t, Tick done);

  BusDevices d_;
};

}  // namespace reality
