/* Relais — unit tests of the key rotation in operation (SEC-07, idea 32).
 *
 * The exchange that produces the keys is tested in test_keys.c; this file tests
 * what happens to a live link when the key changes underneath it:
 *   SEC-07  the sender signs with the current key, the receiver accepts the
 *           current one and the staged one during the transition window, then
 *           the staged key becomes current;
 *   SEC-07  no tick is lost by the rotation - the property is measured over 300
 *           ticks of real frames, not asserted by construction;
 *   SEC-02  once the window is closed the retired key no longer opens the link.
 */
#include "test_util.h"

#include <string.h>

#include "relais/keys.h"

#define PSK_A "000102030405060708090a0b0c0d0e0f"
#define PSK_B "0f0e0d0c0b0a09080706050403020100"

#define PSK_C "f0e1d2c3b4a5968778695a4b3c2d1e0f"

static const char RING_BODY[] = "the bytes a control covers";

/* A prepared key ring and the three controls judged against it: k0 is current,
 * k1 is the staged next key, k2 is a key from outside the rotation. Shared by
 * the two rotation tests so that neither re-implements the set-up. */
typedef struct {
  relais_keyring kr;
  relais_key k0, k1, k2;
  uint64_t c0, c1, c2;
  const char *body;
  size_t len;
} ring_t;

static void ring_setup(ring_t *f) {
  t_key(&f->k0, PSK_A);
  t_key(&f->k1, PSK_B);
  t_key(&f->k2, PSK_C);
  relais_keyring_init(&f->kr, &f->k0);
  f->body = RING_BODY;
  f->len = sizeof RING_BODY - 1u;
  f->c0 = relais_check_compute(&f->k0, f->body, f->len);
  f->c1 = relais_check_compute(&f->k1, f->body, f->len);
  f->c2 = relais_check_compute(&f->k2, f->body, f->len);
}

/* The two-slot key set on its own: what a receiver accepts, and when. */
void test_sec07_rotation_accepts_both_keys_inside_the_window(void) {
  t_section("SEC-07 the receiver accepts both keys during the window");
  ring_t f;
  ring_setup(&f);
  T_CHECK(!f.kr.staged);
  T_EQ_U(relais_keyring_rx(&f.kr, f.body, f.len, f.c0), 0);
  T_EQ_U(relais_keyring_rx(&f.kr, f.body, f.len, f.c1), -1); /* nothing staged yet */

  T_CHECK(relais_keyring_stage(&f.kr, &f.k1, RELAIS_KEYRING_WINDOW));
  T_CHECK(!relais_keyring_stage(&f.kr, &f.k2, RELAIS_KEYRING_WINDOW)); /* one at a time */
  T_CHECK(!relais_keyring_stage(&f.kr, NULL, RELAIS_KEYRING_WINDOW));
  relais_key disabled;
  relais_key_clear(&disabled);
  T_CHECK(!relais_keyring_stage(&f.kr, &disabled, RELAIS_KEYRING_WINDOW));
  T_EQ_U(relais_keyring_rx(&f.kr, f.body, f.len, f.c0), 0); /* the sender has not switched */
  T_EQ_U(relais_keyring_rx(&f.kr, f.body, f.len, f.c1), 1); /* or already has: both pass */
  T_EQ_U(relais_keyring_rx(&f.kr, f.body, f.len, f.c2), -1);
  T_EQ_U(relais_keyring_rx(&f.kr, f.body, f.len, f.c0 ^ 1u), -1);
  T_EQ_U(relais_keyring_rx(NULL, f.body, f.len, f.c0), -1);
}

/* The window is counted in ticks; closing it is what retires the old key. */
void test_sec07_rotation_window_closes_and_promotion_switches(void) {
  t_section("SEC-07 the window closes on ticks, promotion does not wait");
  ring_t f;
  ring_setup(&f);
  T_CHECK(relais_keyring_stage(&f.kr, &f.k1, RELAIS_KEYRING_WINDOW));
  for (uint32_t i = 0; i < RELAIS_KEYRING_WINDOW - 1u; i++) relais_keyring_advance(&f.kr);
  T_CHECK(f.kr.staged);
  T_EQ_U(relais_keyring_rx(&f.kr, f.body, f.len, f.c1), 1);
  relais_keyring_advance(&f.kr);
  T_CHECK(!f.kr.staged);
  T_EQ_U(f.kr.rotations, 1u);
  T_EQ_U(relais_keyring_rx(&f.kr, f.body, f.len, f.c1), 0);  /* the next key is current */
  T_EQ_U(relais_keyring_rx(&f.kr, f.body, f.len, f.c0), -1); /* the old one is gone */
  T_CHECK(memcmp(relais_keyring_tx(&f.kr)->bytes, f.k1.bytes, 16) == 0);
  relais_keyring_advance(&f.kr); /* nothing staged: a tick changes nothing */
  T_EQ_U(f.kr.rotations, 1u);

  /* An explicit promotion is the same switch, without waiting for the window. */
  T_CHECK(relais_keyring_stage(&f.kr, &f.k2, RELAIS_KEYRING_WINDOW));
  T_CHECK(relais_keyring_promote(&f.kr));
  T_EQ_U(f.kr.rotations, 2u);
  T_CHECK(!relais_keyring_promote(&f.kr)); /* nothing left to promote */
  T_EQ_U(relais_keyring_rx(&f.kr, f.body, f.len, f.c2), 0);
  T_EQ_U(relais_keyring_rx(&f.kr, f.body, f.len, f.c1), -1);
  T_CHECK(relais_keyring_tx(NULL) == NULL);
  relais_keyring_advance(NULL);
  relais_keyring_clear(&f.kr);
  T_CHECK(!f.kr.current.enabled && !f.kr.next.enabled && !f.kr.staged);
}

/* A peer that switched before the window closed: its frames are taken, which is
 * what keeps the two ends from having to change key on the very same tick. */
static bool early_switch_accepted(relais_keyring *rx, const relais_msg *order,
                                  const relais_key *next) {
  uint8_t buf[RELAIS_FRAME_MAX];
  relais_msg out;
  relais_reject why = RELAIS_REJ_NONE;
  const size_t n = relais_frame_encode(order, next, buf, sizeof buf);
  return n > 0 && relais_keyring_decode(rx, buf, n, &out, &why);
}

/* SEC-07: the property the mission asks for - a rotation in mid-flight costs
 * nothing. Both ends stage the same next key and count the window in lockstep,
 * and every one of the 300 ticks is decoded by the receiver. */
void test_sec07_rotation_loses_no_tick(void) {
  t_section("SEC-07 rotation in operation: no tick lost");
  relais_key k0, k1;
  t_key(&k0, PSK_A);
  t_key(&k1, PSK_B);
  relais_keyring tx, rx;
  relais_keyring_init(&tx, &k0);
  relais_keyring_init(&rx, &k0);
  relais_params p;
  relais_params_default(&p);
  relais_msg order, out;
  relais_reject why = RELAIS_REJ_NONE;
  uint8_t buf[RELAIS_FRAME_MAX], stale[RELAIS_FRAME_MAX];
  const relais_tick first = 1000, turn = first + 50, last = first + 300;
  long decoded = 0;
  size_t stale_len = 0;
  for (relais_tick tick = first; tick < last; tick++) {
    if (tick == turn) {
      T_CHECK(relais_keyring_stage(&tx, &k1, RELAIS_KEYRING_WINDOW));
      T_CHECK(relais_keyring_stage(&rx, &k1, RELAIS_KEYRING_WINDOW));
    }
    t_order(&order, RELAIS_NODE_A, 1u, tick, 0.25f, &p);
    const size_t n = relais_frame_encode(&order, relais_keyring_tx(&tx), buf, sizeof buf);
    T_CHECK(n > 0);
    if (tick == turn) memcpy(stale, buf, n), stale_len = n; /* signed with k0 */
    if (relais_keyring_decode(&rx, buf, n, &out, &why)) {
      decoded++;
      T_EQ_U(out.tick, tick);
      T_EQ_U(why, RELAIS_REJ_NONE);
    }
    if (tick == turn + 1) T_CHECK(early_switch_accepted(&rx, &order, &k1));
    relais_keyring_advance(&tx);
    relais_keyring_advance(&rx);
  }
  T_EQ_U((uint64_t)decoded, 300u); /* every tick got through the rotation */
  T_EQ_U(tx.rotations, 1u);
  T_EQ_U(rx.rotations, 1u);
  T_CHECK(memcmp(relais_keyring_tx(&tx)->bytes, k1.bytes, 16) == 0);
  /* After the window the key that signed before the rotation is refused. */
  T_CHECK(!relais_keyring_decode(&rx, stale, stale_len, &out, &why));
  T_EQ_U(why, RELAIS_REJ_CHECK);
  relais_keyring_clear(&tx);
  relais_keyring_clear(&rx);
}

int main(void) {
  test_sec07_rotation_accepts_both_keys_inside_the_window();
  test_sec07_rotation_window_closes_and_promotion_switches();
  test_sec07_rotation_loses_no_tick();
  return t_summary();
}
