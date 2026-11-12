/* Relais on FreeRTOS — what configASSERT does when an assumption breaks.
 *
 * The protocol's posture is "stop rather than limp on" (spec 7.2): a violated
 * kernel assumption must be visible, so this halts the tasks with interrupts
 * masked and leaves the machine in the safe state the actuator's fallback law
 * already provides (ACT-03). No file, no console driver dependency.
 */
#include <FreeRTOS.h>
#include <task.h>

void relais_assert_failed(const char *file, int line) {
  (void)file;
  (void)line;
  taskDISABLE_INTERRUPTS();
  for (;;) {
    /* Held here on purpose: a watchdog reset is the intended way out. */
  }
}
