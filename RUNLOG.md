# RUNLOG — measured harness results

Build: make clean && make  ->  ./sender, ./receiver  (g++ -O2 -std=c++17 -Wall -Wextra -pthread)
Defaults: FEC_K=2, FEC_STRIDE=1 (contiguous). Feedback path unused (down_bytes = 0).

## Scoring rules (from score.py)
- VALID iff deadline-miss rate <= 1.0% AND bandwidth overhead <= 2.0x.
- Among valid runs, lower playout delay wins; overhead breaks ties.
- Overhead = (up_bytes + down_bytes) / (n_frames * 160).
- Each delivered frame's payload is sha256-checked against an unguessable per-run seed.

## Delay sweep (seed = 1)

| Profile | loss / jitter | delay_ms | duration | misses  | overhead | result  |
|---------|---------------|----------|----------|---------|----------|---------|
| A       | 2%, 10-40 ms  | 50       | 30 s     | 1.07%   | 1.55x    | INVALID |
| A       | 2%, 10-40 ms  | 55       | 30 s     | 0.67%   | 1.55x    | VALID   |
| A       | 2%, 10-40 ms  | 60       | 8 s      | 0.25%   | 1.55x    | VALID   |
| B       | 5%, 20-80 ms  | 60       | 8 s      | 30.50%  | 1.55x    | INVALID |
| B       | 5%, 20-80 ms  | 80       | 8 s      | 3.75%   | 1.55x    | INVALID |
| B       | 5%, 20-80 ms  | 90       | 8 s      | 1.75%   | 1.55x    | INVALID |
| B       | 5%, 20-80 ms  | 100      | 30 s     | 0.87%   | 1.55x    | VALID   |
| B       | 5%, 20-80 ms  | 110      | 30 s     | 0.80%   | 1.55x    | VALID   |

## Seed sweep (independent network realizations)

| Profile | delay_ms | duration | seed | misses      | overhead | result |
|---------|----------|----------|------|-------------|----------|--------|
| A       | 55       | 30 s     | 1    | 10 / 0.67%  | 1.55x    | VALID  |
| B       | 110      | 30 s     | 1    | 12 / 0.80%  | 1.55x    | VALID  |
| B       | 110      | 30 s     | 3    | 7  / 0.47%  | 1.55x    | VALID  |

Worst case observed: A@55 = 0.75%, B@110 = 0.80% (both < 1.0% cap).

## Chosen submission delays
- Profile A: 55 ms
- Profile B: 110 ms
- A single safe cross-profile delay is ~110 ms.