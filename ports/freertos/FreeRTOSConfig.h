/* Relais on FreeRTOS — kernel configuration (mission M32, idea 73).
 *
 * Three choices here are protocol choices, not taste:
 *
 *  1. configSUPPORT_STATIC_ALLOCATION = 1 and DYNAMIC = 0. Every queue and task
 *     buffer of the port is a static object, so the "no allocation" rule that
 *     core/ respects is true of the whole image (QNX-01, CMD-04).
 *  2. configMAX_PRIORITIES = 8, so the three Relais tasks can sit at 7/6/5 —
 *     the image of the QNX 60/61/62 ordering — above any application task.
 *  3. configUSE_TIME_SLICING = 0. The tasks are peers of one protocol, not
 *     tenants of a CPU: a tick is never cut in the middle by a round robin.
 *
 * The Cortex-M interrupt-priority block at the end is the standard one for the
 * GCC ARM_CM4F port; another port needs its own equivalent (docs/PORT-FREERTOS.md).
 */
#ifndef RELAIS_FREERTOS_CONFIG_H
#define RELAIS_FREERTOS_CONFIG_H

#define configUSE_PREEMPTION                    1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION  1
#define configUSE_IDLE_HOOK                     0
#define configUSE_TICK_HOOK                     0
#define configUSE_MALLOC_FAILED_HOOK            0
#define configCHECK_FOR_STACK_OVERFLOW          2
#define configTICK_RATE_HZ                      ((TickType_t)1000)
#define configMAX_PRIORITIES                    (8)
#define configMINIMAL_STACK_SIZE                ((unsigned short)256)
#define configMAX_TASK_NAME_LEN                 (16)
#define configUSE_16_BIT_TICKS                  0
#define configIDLE_SHOULD_YIELD                 0
#define configUSE_TIME_SLICING                  0

/* Static only: see the header comment. */
#define configSUPPORT_STATIC_ALLOCATION         1
#define configSUPPORT_DYNAMIC_ALLOCATION        0
#define configTOTAL_HEAP_SIZE                   (0)

#define configUSE_MUTEXES                       1
#define configUSE_RECURSIVE_MUTEXES             0
#define configUSE_COUNTING_SEMAPHORES           1
#define configUSE_QUEUE_SETS                    0
#define configUSE_TIMERS                        0
#define configUSE_TRACE_FACILITY                1
#define configGENERATE_RUN_TIME_STATS           0
#define configUSE_CO_ROUTINES                   0

/* The protocol needs the queue API and nothing else from the optional set. */
#define INCLUDE_vTaskPrioritySet                0
#define INCLUDE_uxTaskPriorityGet               1
#define INCLUDE_vTaskDelete                     0
#define INCLUDE_vTaskSuspend                    1
#define INCLUDE_xTaskDelayUntil                 1
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_xTaskGetSchedulerState          1
#define INCLUDE_xQueueGetMutexHolder            0
#define INCLUDE_uxTaskGetStackHighWaterMark     1

/* A violated assumption must stop the machine, not limp on: the same posture as
 * the core's invariants (spec 7.2). */
extern void relais_assert_failed(const char *file, int line);
#define configASSERT(x)                                        \
  do {                                                         \
    if (!(x)) relais_assert_failed(__FILE__, __LINE__);        \
  } while (0)

/* --- Cortex-M4 (GCC ARM_CM4F port) interrupt priorities --- */
#ifdef __NVIC_PRIO_BITS
#define configPRIO_BITS __NVIC_PRIO_BITS
#else
#define configPRIO_BITS 4
#endif
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY       15
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY  5
#define configKERNEL_INTERRUPT_PRIORITY \
  (configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))
#define configMAX_SYSCALL_INTERRUPT_PRIORITY \
  (configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))

#endif /* RELAIS_FREERTOS_CONFIG_H */
