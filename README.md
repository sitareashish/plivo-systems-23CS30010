# Plivo Systems Challenge — Real-Time UDP Playout (23CS30010)

My solution to the "flaky network" problem: get a 20ms audio-ish frame stream across a
lossy, jittery, reordering UDP relay and play it out on time, without an ACK loop and
without blowing the 2x bandwidth budget.

## The problem, quickly

A source produces one 164-byte frame every 20ms (4-byte big-endian seq + 160-byte payload).
The frames go sender -> relay -> receiver -> player. The relay drops, delays, reorders and
duplicates packets. Frame i only counts if it reaches the player before its deadline:
deadline(i) = T0 + DELAY_MS/1000 + i * 0.020

A run is graded VALID only if fewer than 1% of frames miss their deadline AND total wire
traffic stays under 2x the raw payload bytes. Among valid runs, the lowest DELAY_MS wins.

## How I approached it

### Sender: forward error correction, no feedback
My first instinct was to just retransmit dropped packets, but a NAK has to cross the same
broken relay and come back, and at 20ms per frame that round trip eats the whole budget. So
I spend bandwidth up front instead of spending time.

For every block of K = 2 frames I send the two normal DATA packets plus one FEC packet that
is the XOR of the two payloads. If any single packet in the block goes missing, the receiver
rebuilds it by XOR-ing the ones that survived. That works out to a constant ~1.55x overhead,
comfortably under the 2x cap.

I also tried the obvious "just put frame i and frame i-1 in every packet" idea and dropped it:
the scorer measures overhead against the raw 160-byte payload stream, so two payloads per
packet is already 2.0x before you add a single header byte. It literally cannot pass.

### Receiver: jitter buffer + a dedicated playout thread
Two threads:

- Receive thread is the only one touching the socket. It drops each payload into a
  sequence-indexed ring buffer and runs FEC recovery. Reordering and duplicates just fall
  out naturally (first write for a seq wins).
- Playout thread walks sequence numbers in order and, for each frame, sleeps on a
  condition variable until either the frame shows up or we hit ~2ms before its deadline,
  then ships it to the player.

Going strictly in order with per-frame deadlines is self-clocking: a frame that never arrives
can't hold up the frames behind it, because each one has its own wall-clock deadline.

## Build & run

    make
    python3 run.py --profile profiles/A.json --delay_ms 55
    python3 run.py --profile profiles/B.json --delay_ms 110

The sender and receiver read T0, DURATION_S and DELAY_MS from the environment (the
harness sets these). Two optional knobs:

- FEC_K (default 2) — frames per FEC block.
- FEC_STRIDE (default 1) — interleave distance; see the note below on why I left it at 1.

## Results

Numbers below are straight from the harness scorer. Overhead is a flat 1.55x everywhere
because the FEC rate is fixed.

| Profile | Loss / jitter   | DELAY_MS | Seed | Miss rate | Overhead | Result  |
|---------|-----------------|----------|------|-----------|----------|---------|
| A       | 2%, 10-40ms     | 50       | 1    | 1.07%     | 1.55x    | invalid |
| A       | 2%, 10-40ms     | 55       | 1    | 0.67%     | 1.55x    | VALID   |
| B       | 5%, 20-80ms     | 90       | 1    | 1.75%     | 1.55x    | invalid |
| B       | 5%, 20-80ms     | 100      | 1    | 0.87%     | 1.55x    | VALID   |
| B       | 5%, 20-80ms     | 110      | 1    | 0.80%     | 1.55x    | VALID   |
| B       | 5%, 20-80ms     | 110      | 3    | 0.47%     | 1.55x    | VALID   |

The delays I'd submit: 55ms for A, 110ms for B. If I had to pick one number for both,
110ms is safe.

## Two things I learned the hard way

The delay floor is physics, not code. Profile B holds packets for up to 80ms, so no
matter how clever the receiver is, you can't play a frame out at 60ms when the network is
still sitting on it at 80ms. The required delay is basically the network's jitter tail plus
one frame-time for FEC recovery.

Interleaving looked smart but made things worse. I built FEC_STRIDE so a burst would
hit at most one member of a block. Spreading a block over S frames means a recovered frame
has to wait up to S * 20ms for its partner, which pushes it past its deadline. On a mild
burst profile at 60ms, stride 1 gave 1.25% misses and stride 4 gave 3.00%. Since the score
rewards low delay, I ship with stride 1.

## Checking the results are real

The --seed flag controls the relay's entire random pattern, so a single passing run only
proves one network draw. I re-ran B@110 on different seeds (0.80% on seed 1, 0.47% on seed 3)
to make sure it isn't luck. The scorer also sha256-checks every delivered payload against a
per-run secret, so a VALID result means the exact bytes arrived.

## Files

- sender.cpp — FEC sender
- receiver.cpp — jitter buffer + playout receiver
- Makefile — g++ -O2 -std=c++17 -Wall -Wextra -pthread
- NOTES.md — short design writeup
- RUNLOG.md — full delay + seed sweep logs
- SUMMARY.html — one-page visual summary

Everything else in the folder is the provided harness, unchanged.
