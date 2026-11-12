/* Relais — corruption of a frame after authentication: implementation.
 *
 * The flip and the re-signing are two steps kept visible: any caller can flip
 * a frame and re-sign it with a CRC to make a lying component, or just re-sign
 * a frame it already modified bit by bit (SEC-02).
 */
#include "corrupt.h"

size_t fi_corrupt_frame(uint8_t *buf, size_t len, const relais_key *sign_key,
                        fi_zone zone, fi_mode mode, fi_rng *rng, fi_report *rep) {
  if (buf == NULL || sign_key == NULL || len < RELAIS_HEADER_LEN + RELAIS_CHECK_LEN)
    return 0;
  size_t n = fi_flip_frame(buf, len, zone, mode, FI_RANDOM, rng, rep);
  if (n == 0) return 0;
  if (!fi_resign_frame(buf, len, sign_key)) return 0;
  return n;
}

bool fi_resign_frame(uint8_t *buf, size_t len, const relais_key *sign_key) {
  if (buf == NULL || sign_key == NULL || len < RELAIS_HEADER_LEN + RELAIS_CHECK_LEN)
    return false;
  uint64_t check = relais_check_compute(sign_key, buf, len - RELAIS_CHECK_LEN);
  relais_frame_write_u64(buf + len - RELAIS_CHECK_LEN, check);
  return true;
}