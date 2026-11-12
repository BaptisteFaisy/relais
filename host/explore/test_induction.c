/* Relais — the k-induction engine, checked on a property that is deliberately
 * FALSE (spec 7.3, idea 2).
 *
 * A proof engine that proves everything proves nothing. The witness test
 * below feeds the engine two properties that are certainly false -- "the tick
 * number is always even" and "the actuator never loses its holder" -- and
 * demands that the engine REFUSE them, with a counter-example. If the engine
 * ever certifies one of them, the whole machine is worthless and this test
 * says so.
 *
 * The other tests check the mechanism: the situation projection is
 * deterministic and forgets what cannot change a verdict, and the verdict
 * table names the invariants of spec 7.2 one by one.
 */
#include <string.h>

#include "induction.h"
#include "test_util.h"

/* A small configuration: the test must stay fast (the CI budget is five
 * minutes for the whole job). */
static void small_config(ind_config *cfg) {
  ind_config_default(cfg);
  cfg->k = 6u;
  cfg->budget = 20000;
  cfg->class_budget = 4000;
  cfg->faults = 1;
}

static void one_property(const ind_clause *clause, const ind_config *cfg,
                          const relais_params *p, const relais_traj *t, ind_result *res) {
  ind_property prop;
  prop.id = clause->id;
  prop.clauses = clause;
  prop.count = 1;
  T_EQ_U(induction_run(cfg, p, t, &prop, res), 0);
}

static void test_witness_is_refused(void) {
  t_section("a false property must be refused, with a counter-example");
  relais_params p;
  relais_traj t;
  ind_config cfg;
  ind_result res;
  relais_params_default(&p);
  relais_traj_default(&t);
  small_config(&cfg);

  ind_clause even;
  even.id = "witness:tick-always-even";
  even.fn = ind_clause_tick_always_even;
  even.ctx = NULL;
  one_property(&even, &cfg, &p, &t, &res);
  T_CHECK(res.ok);
  T_CHECK(!res.proved); /* the engine must NOT certify a false property */
  T_CHECK(res.first[0] != '\0');

  ind_clause holder;
  holder.id = "witness:holder-never-lost";
  holder.fn = ind_clause_holder_never_lost;
  holder.ctx = NULL;
  one_property(&holder, &cfg, &p, &t, &res);
  T_CHECK(res.ok);
  T_CHECK(!res.proved); /* the actuator does lose its holder when it dies */
  T_CHECK(res.first[0] != '\0');
}

static void test_situation_projection(void) {
  t_section("the situation forgets what cannot change a verdict");
  relais_params p;
  relais_traj t;
  relais_params_default(&p);
  relais_traj_default(&t);

  explore_world w1;
  explore_world_init(&w1, &p, &t, true, true);
  uint64_t h1 = induction_class_hash(&w1);
  uint64_t h2 = induction_class_hash(&w1);
  T_EQ_U(h1, h2); /* the projection is deterministic */

  /* The plant position and the trajectory cannot change a safety verdict
   * (symbolic.h says so): two worlds that differ only by the plant are the
   * same situation. */
  explore_world w2 = w1;
  w2.plant.pos = 0.75f;
  w2.plant.vel = -0.5f;
  T_EQ_U(h1, induction_class_hash(&w2));

  /* A world whose holder died is a different situation. */
  explore_world w3 = w1;
  w3.dead[0] = true;
  T_NE(h1, induction_class_hash(&w3));
}

static void test_safety_is_proved(void) {
  t_section("the safety property of the core is proved for every depth");
  relais_params p;
  relais_traj t;
  ind_config cfg;
  ind_result res;
  relais_params_default(&p);
  relais_traj_default(&t);
  small_config(&cfg);
  /* The fault-free protocol: the set of situations is finite and closes, so
   * the induction removes the bound on the depth (the single-fault set is far
   * bigger; see docs/INDUCTION.md). */
  cfg.faults = 0;
  cfg.class_budget = 20000;

  ind_clause core = ind_clause_core;
  core.id = "safety";
  one_property(&core, &cfg, &p, &t, &res);
  T_CHECK(res.ok);
  T_CHECK(res.closed);       /* the set of situations closed */
  T_CHECK(res.proved);       /* base + step hold: no bound on the depth */
  T_CHECK(res.k_close > 0u); /* the depth at which the induction closes */
  T_CHECK(res.classes > 1);  /* the quotient is not a single class */
  T_CHECK(res.class_transitions > 0);

  ind_verdict verdicts[16];
  size_t n = induction_verdicts(&res, verdicts, 16);
  T_CHECK(n >= 12u); /* I1..I11, DET-03, DET-09 */
  size_t proved = 0;
  for (size_t i = 0; i < n; i++)
    if (verdicts[i].inductive) proved++;
  T_CHECK(proved >= 10u); /* I5 and DET-09 are out of the explorer's reach */
}

static void test_single_fault_stays_bounded(void) {
  t_section("under a fault the engine reports what stays bounded");
  relais_params p;
  relais_traj t;
  ind_config cfg;
  ind_result res;
  relais_params_default(&p);
  relais_traj_default(&t);
  small_config(&cfg);
  cfg.faults = 1;

  ind_clause core = ind_clause_core;
  core.id = "safety";
  one_property(&core, &cfg, &p, &t, &res);
  T_CHECK(res.ok);
  /* The bounded base case (the existing explorer) finds no violation, and the
   * engine says plainly whether the set of situations closed. */
  T_CHECK(!res.base_failed);
  T_CHECK(res.classes > 0);
  if (!res.closed) {
    T_CHECK(!res.proved); /* no unbounded claim without closure */
    T_CHECK(strstr(res.first, "bounded") != NULL); /* and the engine says so */
  }
}

int main(void) {
  test_witness_is_refused();
  test_situation_projection();
  test_safety_is_proved();
  test_single_fault_stays_bounded();
  return t_summary();
}
