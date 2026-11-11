/* Relais — mutants (spec 7.6, INJ-11).
 *
 * A mutant is the very same source, compiled with -DRELAIS_MUTANT=n. The hooks
 * below are the only places where a mutant changes behaviour, so the bench can
 * prove that it sees a fault without shipping a second implementation.
 * In the nominal build every hook folds to a constant and disappears.
 */
#ifndef RELAIS_MUTANT_H
#define RELAIS_MUTANT_H

#define RELAIS_M_NONE                   0
#define RELAIS_M1_ACCEPT_ANY            1 /* actuator applies anybody's order  -> I1 */
#define RELAIS_M2_IGNORE_TICK           2 /* actuator ignores the tick label   -> I3 */
#define RELAIS_M3_GRANT_WITHOUT_SILENCE 3 /* mandate granted with no evidence  -> I7 */
#define RELAIS_M4_TESTAMENT_FOREVER     4 /* will played past its horizon      -> I6 */
#define RELAIS_M5_NO_STATE_IMPORT       5 /* second keeps its own state        -> I5 */
#define RELAIS_M6_MANDATE_RESET         6 /* mandate restarts at zero          -> I2 */
#define RELAIS_M7_ORDER_ON_SUSPICION    7 /* second sends orders without grant -> MAN-08 */
#define RELAIS_M8_LAMP_EARLY_OFF        8 /* lamp off while second uncertified -> I10 */

#ifndef RELAIS_MUTANT
#define RELAIS_MUTANT RELAIS_M_NONE
#endif

#define RELAIS_MUT(n) (RELAIS_MUTANT == (n))
/* Name of the compiled-in mutant, for the report and the journal header. */
const char *relais_mutant_name(void);
int relais_mutant_id(void);

#endif /* RELAIS_MUTANT_H */
