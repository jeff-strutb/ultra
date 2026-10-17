#include <catch2/catch_test_macros.hpp>
#include <vector>

#include "core/scheduler.hpp"

using namespace ultra;

TEST_CASE("scheduler dispatches in tick then schedule order") {
  Scheduler s;
  std::vector<int> log;
  struct Ctx { std::vector<int>* log; int id; };
  Ctx c[3] = {{&log, 0}, {&log, 1}, {&log, 2}};
  Event e[3];
  auto cb = [](void* p, Tick) { auto* x = static_cast<Ctx*>(p); x->log->push_back(x->id); };
  for (int i = 0; i < 3; ++i) s.add(e[i], "e", cb, &c[i]);
  s.schedule(e[0], 10);
  s.schedule(e[1], 5);
  s.schedule(e[2], 10);
  s.run_until(20);
  REQUIRE(log == std::vector<int>{1, 0, 2});
  REQUIRE(s.now() == 20);
}
