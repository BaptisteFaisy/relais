# Plugging a REAL steering dataset into the replay (idea 62)

`dataset_replay.py` reads any CSV with two columns — `time_s` (seconds,
strictly increasing) and `angle` (steering-wheel angle, any unit: the study
normalises it) — and plays it as the protocol setpoint through the real C
arbiter. Nothing here was downloaded for M31; this file documents the
candidates, their licences, and the checks to perform BEFORE any use.

## How to plug a dataset in

1. Obtain the archive through its official channel (never a mirror).
2. Extract the steering channel and the clock:
   - comma10k/comma2k18: CAN packets, message `0x025` (Honda Bosch): field
     `STEER_ANGLE` (deg, signed), 100 Hz; timestamps from the log index.
   - nuScenes: `can_bus.json` of the CAN bus expansion, key `steer`
     (radians, feedback), 50 Hz+; sync with the 20 Hz keyframes.
   - Udacity: `driving_log.csv`, column `steering` (normalised -1..1),
     already per-frame at ~20 Hz; interpolate to 200 Hz.
3. Convert to the two-column CSV (a ten-line Python script with the standard
   library is enough; keep the unit in the file name).
4. Normalise: `angle / max(|angle|)` maps full lock to 1.0, the protocol's
   full scale (ACT-05). Keep the raw archive untouched.
5. Replay:

       python3 host/study/dataset_replay.py --csv <file>.csv \
           --fault <mid>:death:0 --level complet --out runs/m31-dataset

## Candidate public datasets and their licences (to re-verify before use)

| dataset | steering channel | licence (as published) | what to check before use |
|---|---|---|---|
| comma10k (comma.ai) | CAN 0x025 STEER_ANGLE, 100 Hz | released "free for non-commercial use" by comma.ai; no OSI licence | commercial use forbidden; no redistribution of derived data without permission; the repo README is the authority |
| comma2k18 | same CAN bus, longer highway logs | same comma.ai terms | same as comma10k |
| nuScenes + CAN bus expansion | `steer` (rad) at 50 Hz+, keyframes 20 Hz | CC BY-NC-SA 4.0 | non-commercial only; ShareAlike contaminates derivatives; attribution required; faces/plates already anonymised by the publisher — keep it that way |
| Udacity Self-Driving Car (dataset 2) | `steering` normalised, ~20 Hz | published for educational use, no explicit OSI licence on the data | ask before redistribution; low rate needs interpolation (a 20 Hz wheel signal aliases fast manoeuvres — document the smoothing) |
| Waymo Open Dataset | steering angle in vehicle state | Waymo Open Dataset License (non-commercial, no redistribution) | registration + click-through; commercial use forbidden |
| KITTI (raw) | NONE (no steering channel; IMU/odometry only) | CC BY-NC-SA 3.0 | unusable as a steering replay; could serve for disturbance profiles only |
| Argoverse 2 | vehicle pose (no direct wheel angle) | CC BY-NC 4.0 | derive curvature -> angle via a bicycle model: a derivation, not a measurement; say so in the report |

Rules of the house (apply to every row of the table):

1. **Read the licence file in the archive you actually downloaded** — the
   table above is a snapshot, not the authority.
2. **Non-commercial clauses survive derivatives**: a replay journal or a chart
   built from NC data inherits the clause in most jurisdictions; publish
   results as research, or use data you own.
3. **Attribution**: cite the dataset paper and version in the report that
   publishes the numbers.
4. **No redistribution**: commit to this repository only the converter script
   and the provenance (URL, version, checksum), never the data itself; the
   synthetic dataset in `host/study/data/` exists so CI needs no download.
5. **Privacy**: driving data can contain plates/faces/routes. The publishers
   above anonymise; do not undo that, and do not republish raw frames.
6. **Unit sanity**: a degrees/radians mix-up shows up as a 57x error; check
   the first seconds against a video or the published range before replaying.
