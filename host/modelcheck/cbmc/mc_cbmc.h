/* Relais — the CBMC harness helpers (mission M36).
 *
 * The harnesses of host/modelcheck/cbmc/ are plain C11: under CBMC they use
 * __CPROVER_assert and the nondeterministic primitives; under gcc they only
 * have to COMPILE, which is what `make modelcheck` checks on a machine where
 * CBMC is not installed. The property statements do not change with the
 * tool: the same text is asserted by CBMC and read by a human.
 *
 * Requirements cited per property: MAN-02, MAN-03, SEC-01, SEC-02, SEC-03,
 * SEC-04, ACT-01, ACT-05, CMD-06, ROB-09, I1, I3.
 */
#ifndef RELAIS_MC_CBMC_H
#define RELAIS_MC_CBMC_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __CPROVER

#define MC_ASSERT(cond, msg) __CPROVER_assert(cond, msg)
#define MC_ASSUME(cond) __CPROVER_assume(cond)
#define MC_NONDET_U8() __CPROVER_nondet_uchar()
#define MC_NONDET_U32() __CPROVER_nondet_uint()
#define MC_NONDET_U64() __CPROVER_nondet_ulong()
#define MC_NONDET_BOOL() __CPROVER_nondet_bool()
#define MC_NONDET_FLOAT() __CPROVER_nondet_float()

#else /* gcc: the harness must only compile, the values are irrelevant here */

#include <assert.h>

#define MC_ASSERT(cond, msg) assert(cond)
#define MC_ASSUME(cond) ((void)(cond))
#define MC_NONDET_U8() ((uint8_t)0)
#define MC_NONDET_U32() ((uint32_t)0)
#define MC_NONDET_U64() ((uint64_t)0)
#define MC_NONDET_BOOL() (false)
#define MC_NONDET_FLOAT() (0.0f)

#endif

#endif /* RELAIS_MC_CBMC_H */
