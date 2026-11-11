/* Relais — which mutant this build is (spec 7.6). One definition, used by the
 * journal header and by the bench report so a campaign cannot mistake a mutant
 * run for a nominal one. */
#include "relais/mutant.h"

int relais_mutant_id(void) { return RELAIS_MUTANT; }

const char *relais_mutant_name(void) {
  switch (RELAIS_MUTANT) {
    case RELAIS_M1_ACCEPT_ANY: return "M1-accept-any-order";
    case RELAIS_M2_IGNORE_TICK: return "M2-ignore-tick-label";
    case RELAIS_M3_GRANT_WITHOUT_SILENCE: return "M3-grant-without-evidence";
    case RELAIS_M4_TESTAMENT_FOREVER: return "M4-testament-past-horizon";
    case RELAIS_M5_NO_STATE_IMPORT: return "M5-second-keeps-own-state";
    case RELAIS_M6_MANDATE_RESET: return "M6-mandate-restarts-at-zero";
    case RELAIS_M7_ORDER_ON_SUSPICION: return "M7-order-on-suspicion";
    case RELAIS_M8_LAMP_EARLY_OFF: return "M8-lamp-off-too-early";
    default: return "nominal";
  }
}
