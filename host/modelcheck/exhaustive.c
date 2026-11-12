/* Relais — M36: the bounded exhaustive check of the C core.
 *
 * Three spaces, each a FINITE published input space, each case a call to the
 * real core functions. The program publishes the number of cases, the wall time
 * and the bounds: what is proved inside the bounds, and what is NOT covered
 * outside them. See docs/MODELCHECK.md.
 */
#include "mc.h"

#include <stdio.h>

static void print_bounds(void) {
  printf("MC-BOUNDS A: types=%u payload_len=0..%u byte_values=%u extend=%u key_modes=%u "
         "field_values=%u ticks=%u mandates=%u\n",
         MC_A_TYPES, MC_A_PAYLEN_MAX, MC_A_BYTE_VALUES, MC_A_EXTEND_MAX, MC_A_KEY_MODES,
         MC_A_FIELD_VALUES, MC_A_TICKS, MC_A_MANDATES);
  printf("MC-BOUNDS B: states=%u tick_offsets=%u types=%u senders=%u frame_ticks=%u "
         "mandates=%u values=%u pair_vocab=%u pair_ticks=%u seq_vocab=%u seq_depth=%u\n",
         MC_B_STATES, MC_B_TICK_OFFSETS, MC_B_TYPES, MC_B_SENDERS, MC_B_FRAME_TICKS,
         MC_B_MANDATES, MC_B_VALUES, MC_B_PAIR_VOCAB, MC_B_PAIR_TICKS, MC_B_SEQ_VOCAB,
         MC_B_SEQ_DEPTH);
  printf("MC-BOUNDS C: clamp_values=%u amplitudes=%u meas_grid=%u params=%u run_ticks=%u "
         "tst_len=%u tst_bases=%u tst_ticks=%u\n",
         MC_C_CLAMP_VALUES, MC_C_AMPLITUDES, MC_C_MEAS_GRID, MC_C_PARAMS, MC_C_RUN_TICKS,
         MC_C_TST_LEN, MC_C_TST_BASES, MC_C_TST_TICKS);
}

int main(void) {
  double t0 = mc_now();
  printf("M36 bounded exhaustion of the C core\n");
  print_bounds();
  mc_space_wire(&mc_sp_a);
  mc_space_arbiter(&mc_sp_b);
  mc_space_control(&mc_sp_c);
  double total = mc_now() - t0;
  mc_report(&mc_sp_a);
  mc_report(&mc_sp_b);
  mc_report(&mc_sp_c);
  printf("MC-TOTAL cases=%llu time=%.3fs\n", (unsigned long long)mc_total_cases(), total);
  printf("MC-OBSERVATIONS holder_not_computer=%llu demande_from_actuator=%llu "
         "contract_violation=%llu slew_nonfinite=%llu\n",
         (unsigned long long)mc_obs_holder_not_computer,
         (unsigned long long)mc_obs_demande_from_actuator,
         (unsigned long long)mc_obs_contract_violation,
         (unsigned long long)mc_obs_slew_nonfinite);
  return t_summary();
}
