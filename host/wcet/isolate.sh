#!/bin/sh
# Relais — prepare a machine for WCET measurement (mission M40).
#
# WHAT IT DOES. It probes every knob that changes a latency distribution, uses
# the ones that work WITHOUT root, and prints an explicit verdict for the ones
# that do not. Nothing is bypassed: a root-only knob is reported as
# BLOCKED(root) together with the exact command an operator would run.
#
#   [ DONE ]    applied (or readable) in this run, no root needed
#   [ SKIP ]    not applicable on this platform / not requested
#   [BLOCKED]   would change the measurement, needs root or a missing tool
#
# PORTABILITY. POSIX sh + uname/getconf only. On Linux it uses taskset, chrt,
# /sys and /proc; on QNX 8.0 it uses `on -C`, `pidin` and reports the knobs that
# do not exist there; on anything else it degrades to the capability table. The
# same file runs on the PC, on a Raspberry Pi and on the QNX target.
#
# USAGE
#   host/wcet/isolate.sh                     # capability table only
#   host/wcet/isolate.sh --json env.json     # ... plus a machine-readable record
#   host/wcet/isolate.sh --cpu 3 -- CMD ...  # run CMD pinned to CPU 3
#
# Requirements: PERF-01 (the number must be measured under known conditions),
# PERF-06 (isolation from lower-priority load), QNX-03 (priority, locked memory),
# QNX-04 (the same procedure on the target).

set -u

CPU=""
JSON_OUT=""
QUIET=0
CMD=""

# ---------------------------------------------------------------- reporting
say() { [ "$QUIET" -eq 1 ] || printf '%s\n' "$*"; }
row() { [ "$QUIET" -eq 1 ] || printf '  [%-7s] %-24s %s\n' "$1" "$2" "$3"; }
json_buf=""

jadd() { # jadd key value   (value is emitted verbatim: number, true/false, "s")
  if [ -z "$json_buf" ]; then json_buf="\"$1\":$2"; else json_buf="$json_buf,\"$1\":$2"; fi
}
jstr() { jadd "$1" "\"$(printf '%s' "$2" | tr -d '"\\' | tr '\n' ' ')\""; }

have() { command -v "$1" >/dev/null 2>&1; }

# --------------------------------------------------------------- platform
OS=$(uname -s 2>/dev/null || echo unknown)
KERNEL=$(uname -r 2>/dev/null || echo unknown)
MACHINE=$(uname -m 2>/dev/null || echo unknown)
HOSTNAME_S=$(uname -n 2>/dev/null || echo unknown)
NCPU=$( (nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null) | head -1)
[ -n "$NCPU" ] || NCPU=1
MODEL=""
if [ -r /proc/cpuinfo ]; then
  MODEL=$(sed -n 's/^[Mm]odel name[[:space:]]*:[[:space:]]*//p' /proc/cpuinfo | head -1)
  [ -n "$MODEL" ] || MODEL=$(sed -n 's/^[Hh]ardware[[:space:]]*:[[:space:]]*//p' /proc/cpuinfo | head -1)
fi
[ -n "$MODEL" ] || MODEL=$(uname -p 2>/dev/null || echo "$MACHINE")

# ------------------------------------------------------------------- args
while [ $# -gt 0 ]; do
  case "$1" in
    --cpu) CPU="$2"; shift 2 ;;
    --json) JSON_OUT="$2"; shift 2 ;;
    --quiet) QUIET=1; shift ;;
    --) shift; CMD="$*"; break ;;
    -h|--help) sed -n '2,30p' "$0"; exit 0 ;;
    *) echo "isolate.sh: unknown option $1" >&2; exit 2 ;;
  esac
done

say "relais isolate.sh — measurement machine preparation (M40)"
say "  host=$HOSTNAME_S os=$OS kernel=$KERNEL machine=$MACHINE cpus=$NCPU"
say "  cpu=$MODEL"
say "  running as uid=$(id -u 2>/dev/null || echo '?') (root=$( [ "$(id -u 2>/dev/null || echo 1)" = 0 ] && echo yes || echo no))"
LOADAVG=$( (cut -d' ' -f1-3 /proc/loadavg 2>/dev/null) || echo "n/a")
say "  loadavg=$LOADAVG"
jstr host "$HOSTNAME_S"; jstr os "$OS"; jstr kernel "$KERNEL"
jstr machine "$MACHINE"; jstr cpu_model "$MODEL"
jadd cpus "$NCPU"; jstr loadavg "$LOADAVG"
jadd uid "$(id -u 2>/dev/null || echo 0)"

# ------------------------------------------------- 1. CPU affinity (no root)
if [ -n "$CPU" ]; then
  if [ "$OS" = "QNX" ] && have on; then
    row DONE "affinity" "will run under 'on -C $CPU' (QNX)"
    jstr affinity "on -C $CPU"
  elif have taskset; then
    if taskset -c "$CPU" true 2>/dev/null; then
      row DONE "affinity" "taskset -c $CPU (own process, no root needed)"
      jstr affinity "taskset -c $CPU"
    else
      row BLOCKED "affinity" "taskset refused CPU $CPU (mask not permitted)"
      jstr affinity "blocked"
    fi
  else
    row BLOCKED "affinity" "no taskset/on: cannot pin (install util-linux)"
    jstr affinity "unavailable"
  fi
else
  row SKIP "affinity" "no --cpu given: the probe runs on the default mask"
  jstr affinity "none"
fi

# ------------------------------------------- 2. real-time priority (no root?)
RT_SOFT=$(ulimit -r 2>/dev/null || echo "?")
if [ "$OS" = "QNX" ]; then
  if have on; then
    row DONE "rt priority" "QNX: 'on -p PRIO cmd' raises priority without root"
    jstr rt_priority "on -p (qnx)"
  else
    row SKIP "rt priority" "QNX without 'on'"
    jstr rt_priority "unavailable"
  fi
elif have chrt; then
  if chrt -f 1 true 2>/dev/null; then
    row DONE "rt priority" "SCHED_FIFO allowed without root (RLIMIT_RTPRIO=$RT_SOFT)"
    jstr rt_priority "allowed"
  else
    row BLOCKED "rt priority" "SCHED_FIFO refused: RLIMIT_RTPRIO=$RT_SOFT (root: \
'ulimit -r 99' in the shell, or /etc/security/limits.conf rtprio, or run as root)"
    jstr rt_priority "blocked"
  fi
else
  row SKIP "rt priority" "no chrt: the probe reports sched_fifo=false"
  jstr rt_priority "unavailable"
fi
jadd rtprio_soft "$(printf '%s' "$RT_SOFT" | tr -cd '0-9' | sed 's/^$/0/')"

# ------------------------------------------------------- 3. locked memory
MEMLOCK=$(ulimit -l 2>/dev/null || echo "?")
case "$MEMLOCK" in
  unlimited) row DONE "memory lock" "RLIMIT_MEMLOCK=unlimited: mlockall will succeed" ;;
  0) row BLOCKED "memory lock" "RLIMIT_MEMLOCK=0: mlockall will fail (root: ulimit -l unlimited)" ;;
  *) row DONE "memory lock" "RLIMIT_MEMLOCK=${MEMLOCK} KiB: probe --lock reports the truth" ;;
esac
jstr memlock_kib "$MEMLOCK"

# -------------------------------------------------- 4. frequency governor
GOV_PATH=/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
FREQ_PATH=/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq
if [ -r "$GOV_PATH" ]; then
  GOV=$(cat "$GOV_PATH")
  CUR="n/a"
  [ -r "$FREQ_PATH" ] && CUR="$(cat "$FREQ_PATH") kHz"
  if [ -w "$GOV_PATH" ]; then
    row DONE "governor" "$GOV ($CUR) and the sysfs node is writable"
    jstr governor "$GOV(writable)"
  else
    row BLOCKED "governor" "$GOV ($CUR); a WCET run wants 'performance': \
root# cpupower frequency-set -g performance  (or echo performance > $GOV_PATH)"
    jstr governor "$GOV(readonly)"
  fi
  jstr scaling_cur_freq "$CUR"
elif [ "$OS" = "QNX" ]; then
  row SKIP "governor" "QNX: no cpufreq sysfs; the SoC clock is fixed by the BSP"
  jstr governor "qnx-fixed"
else
  row SKIP "governor" "no cpufreq sysfs on this machine"
  jstr governor "absent"
fi
BOOST=/sys/devices/system/cpu/intel_pstate/no_turbo
if [ -r "$BOOST" ]; then
  row BLOCKED "turbo/boost" "no_turbo=$(cat "$BOOST") (root: echo 1 > $BOOST to freeze \
the frequency and kill a source of dispersion)"
  jstr no_turbo "$(cat "$BOOST")"
fi

# --------------------------------------------- 5. kernel-side isolation
if [ -r /proc/cmdline ]; then
  CMDLINE=$(cat /proc/cmdline)
  case "$CMDLINE" in
    *isolcpus*) row DONE "isolcpus" "present in the kernel command line" ;;
    *) row BLOCKED "isolcpus" "absent: other processes share every CPU (root: add \
'isolcpus=nohz,domain,N quiet' to the kernel command line and reboot)" ;;
  esac
  jstr kernel_cmdline "$CMDLINE"
else
  row SKIP "isolcpus" "/proc/cmdline unreadable on $OS"
  jstr kernel_cmdline "unreadable"
fi
THP=/sys/kernel/mm/transparent_hugepage/enabled
if [ -r "$THP" ]; then
  row BLOCKED "huge pages" "$(cat "$THP") (root: echo never > $THP removes a \
compaction stall from the tail)"
  jstr thp "$(cat "$THP")"
fi
ASLR=/proc/sys/kernel/randomize_va_space
if [ -r "$ASLR" ]; then
  jstr aslr "$(cat "$ASLR")"
  row SKIP "aslr" "randomize_va_space=$(cat "$ASLR") (root may set 0; the probe is \
position independent, so this only shifts the layout)"
fi

# --------------------------------------------------------- 6. cache + load
if [ -r /sys/devices/system/cpu/cpu0/cache/index2/size ]; then
  row DONE "cache" "L2=$(cat /sys/devices/system/cpu/cpu0/cache/index2/size); the probe \
warms every path for --warmup ticks before recording"
  jstr cache_l2 "$(cat /sys/devices/system/cpu/cpu0/cache/index2/size)"
elif have lscpu; then
  row DONE "cache" "$(lscpu 2>/dev/null | sed -n 's/^L2 cache:[[:space:]]*//p' | head -1)"
  jstr cache_l2 "lscpu"
else
  row SKIP "cache" "no cache description readable on $OS"
  jstr cache_l2 "unknown"
fi
NPROC_RUN=$(ps -eo state 2>/dev/null | grep -c '^R' || echo "?")
say "  running processes: $NPROC_RUN (a busy machine widens the tail: re-run when quiet)"
jadd running_procs "$(printf '%s' "$NPROC_RUN" | tr -cd '0-9' | sed 's/^$/0/')"

# -------------------------------------------------------- 7. run the command
if [ -n "$CMD" ]; then
  WRAP=""
  if [ -n "$CPU" ]; then
    if [ "$OS" = "QNX" ] && have on; then WRAP="on -C $CPU"
    elif have taskset; then WRAP="taskset -c $CPU"; fi
  fi
  say "  exec: $WRAP $CMD"
  # shellcheck disable=SC2086
  exec $WRAP sh -c "$CMD"
fi

if [ -n "$JSON_OUT" ]; then
  printf '{%s}\n' "$json_buf" > "$JSON_OUT" || {
    echo "isolate.sh: cannot write $JSON_OUT" >&2; exit 1; }
  say "  environment record written to $JSON_OUT"
fi
exit 0
