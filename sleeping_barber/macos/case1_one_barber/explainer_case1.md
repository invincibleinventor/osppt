# Sleeping Barber — Case 1 (1 barber, 3 chairs, priority + FCFS)

## The classic problem, extended with priority

A barbershop has one barber and a waiting room with a fixed number of
chairs (`CHAIRS = 3`). If no customers are around, the barber sleeps.
When a customer arrives:

- If the waiting room has a free chair, they sit down.
- If the waiting room is full, the customer leaves.

This version adds a twist: each customer has a **priority**. When the
barber is ready for the next customer, it doesn't just pick whoever sat
down first — it picks whoever is currently seated with the **highest
priority**. If two seated customers share the same priority, the one who
sat down earlier wins (FCFS tie-break). This models a real queue where,
say, walk-in appointments jump ahead of standard ones, but two standard
customers are still served in arrival order.

## The synchronization tools

| Name              | Type              | Starts at | Purpose                                             |
|-------------------|-------------------|-----------|------------------------------------------------------|
| `waiting_manager` | named semaphore   | 0         | Customer → barber: "someone is waiting" (lets the barber sleep at zero CPU until posted) |
| per-customer `turn`| named semaphore  | 0         | Barber → *that specific* customer: "it's your turn now" |
| `chair_lock`      | mutex             | unlocked  | Protects the `chairs[]` array and `arrival_seq`      |

Each customer opens its own uniquely-named `turn` semaphore
(`/sb_case1_turn_<id>`) so the barber can wake exactly the customer it
picked, not just whoever happens to be first in line.

## Code shape

The file is split into two halves:

1. **Core synchronization logic** (top): `barber()`, `customer()`, `main()`.
2. **Visualization / I/O** (bottom): colors, timestamps, log files, and
   spawning the terminal windows. None of this affects correctness.

## Walkthrough

**`customer()`**:
1. Opens its personal `turn` semaphore.
2. Locks `chair_lock`, looks for a free chair.
3. If the room is full: unlocks and leaves (logs a rejection).
4. If a chair is free: claims it, records `priority`, and stamps
   `seq = arrival_seq++` — a monotonically increasing arrival counter
   taken *while holding the lock*, so it's race-free even when several
   customers sit down in the same instant. `seq` is what breaks ties
   between equal priorities later.
5. Unlocks, posts `waiting_manager` (wakes the barber, or just
   increments the count if it's busy), then blocks on its own `turn`
   semaphore until the barber calls it by name.

**`barber()`** loops forever:
1. Logs that it's asleep, blocks on `sem_wait(waiting_manager)` — the
   actual "sleep": zero CPU until a customer posts.
2. On waking, locks `chair_lock` and scans all occupied chairs for a
   **winner**: the highest `priority`, and among ties, the lowest `seq`
   (earliest arrival). If nothing is occupied, this wake-up was the
   shutdown signal — break and go home.
3. If a lower-priority customer was skipped over, logs that fact
   explicitly (the "prioritized ahead of" line).
4. Frees the winner's chair *immediately* (the chair models the waiting
   room, not the barber's own chair — the customer has now moved), then
   posts that customer's personal `turn` semaphore.
5. Sleeps 2 seconds to simulate the haircut, then loops back.

## Interactive menu: seeding + rounds

Instead of a fixed scripted sequence, `main()` asks two things:

1. **Initial occupancy** — how many customers are already seated when
   the shop "opens" (0 to `CHAIRS`), with a priority prompt for each.
   Seeding `CHAIRS - 1` here leaves exactly one vacancy.
2. **Rounds** — repeatedly asks how many customers arrive together this
   round. All `n` of that round's customer threads are spawned
   back-to-back with no delay between them, so contention is genuine,
   not scripted:
   - `n == 1` — a plain arrival.
   - `n >= 2` — those threads race each other for whatever chairs are
     actually free at that instant. If only one vacancy exists, exactly
     one of them wins it and the other logs a rejection — this is the
     "one vacancy, two customers fighting for it" scenario.
   - `n == 0` — closes the shop and ends the round loop.

Because the mutex serializes the check-and-claim sequence in
`customer()`, the outcome is always correct (chairs are never
double-assigned) even though *who wins* when several race is left to
the OS scheduler.

## Priority-jump demonstration

Seed one or two normal-priority customers, then in a round send a
customer with a higher priority. The barber's winner-selection logs a
"Customer X is prioritized ahead of Customer Y" line the next time it
wakes, showing the higher-priority arrival being served first even
though it sat down later.

## Shutdown handshake

After all customer threads are joined, `main()` posts `waiting_manager`
one final time. The barber, asleep on that semaphore, wakes up, finds no
occupied chairs, and exits its loop instead of sleeping forever.

## Why named semaphores (`sem_open`), not `sem_init`

Plain unnamed POSIX semaphores (`sem_init`/`sem_destroy`) are deprecated
and, in testing, unreliable on macOS. Named semaphores via `sem_open`
give identical `sem_wait`/`sem_post` semantics but behave correctly.
Each semaphore name is unlinked before creation and after use, so
back-to-back runs never collide on a stale name.

## The output: 3 live terminal windows, auto-tiled (macOS only)

Each conceptual channel gets its own window:

- **Waiting Room** — a live ASCII box showing which of the 3 chairs are
  occupied (color-coded normal vs. priority) or free, redrawn on every
  change.
- **Barber Shop** — the barber's own narration: asleep / calls customer
  / finishes haircut.
- **Transactions** — every arrival, seating, prioritization, and
  rejection, in order.

On macOS, `setup_display()` queries the screen's pixel size via
`osascript` (Finder's desktop window bounds) once, then positions each
spawned Terminal.app window into its own screen quadrant (top-left,
top-right, bottom-left) so the three windows auto-tile instead of
stacking on top of each other. Each channel writes to its own log file
with `fflush()` after every line; the spawned terminal runs `tail -f` on
that file so updates appear live.
