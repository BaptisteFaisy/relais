/* Relais — corruption of a frame AFTER authentication: the channel
 * looks clean because the trailer is recomputed. This is the most dangerous
 * fault of the model: a noisy channel is unmasked by the CRC, a lying component
 * is not -- only the keyed MAC at level C tells the two apart (SEC-01, SEC-02, DET-04).
 */
#ifndef RELAIS_FAULTINJECT_CORRUPT_H
#define RELAIS_FAULTINJECT_CORRUPT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bitflip.h"

/* Flip one body grain (bit/byte/word) of an encoded frame in one zone, then
 * recompute the trailer with sign_key so it parses as valid. With a CRC key
 * (enabled == false) the frame passes level S; a keyed MAC computed over the
 * same bytes still rejects it. Returns the number of bytes inverted. */
size_t fi_corrupt_frame(uint8_t *buf, size_t len, const relais_key *sign_key,
                        fi_zone zone, fi_mode mode, fi_rng *rng, fi_report *rep);

/* Recompute the trailer of an already modified frame with sign_key. Used by
 * the tests and the campaign to turn a raw flip into a lying component. */
bool fi_resign_frame(uint8_t *buf, size_t len, const relais_key *sign_key);

#endif /* RELAIS_FAULTINJECT_CORRUPT_H */