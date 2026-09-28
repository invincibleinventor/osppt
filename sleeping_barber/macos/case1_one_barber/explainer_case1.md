# Sleeping Barber — Case 1 (1 barber, 3 chairs, 3 customers)

## The classic problem

A barbershop has one barber, one barber chair, and a waiting room with a
fixed number of chairs (here: 3). If no customers are around, the barber
sleeps. When a customer arrives:

- If the waiting room has a free chair, they sit and wake the barber if
  he's asleep.
- If the waiting room is full, the customer leaves.

The barber serves one customer at a time and goes back to sleep when the
room is empty. The problem is a classic producer/consumer synchronization
exercise: customers "produce" work, the barber "consumes" it, and a
shared waiting-room count must never be corrupted by concurrent access.

## The synchronization tools (exactly what the problem needs, no more)

| Name              | Type              | Starts at | Purpose                                             |
|-------------------|-------------------|-----------|------------------------------------------------------|
| `waiting_manager` | counting semaphore| 0         | Customer → barber: "someone is waiting" (this is what lets the barber *sleep* with zero CPU until posted) |
| `barber_ready`    | counting semaphore| 0         | Barber → customer: "it's your turn now"             |
| `chair_lock`      | mutex             | unlocked  | Protects the shared `chairs_used` counter            |

Three synchronization primitives total, matching the problem statement:
1 barber, 1 "waiting manager" semaphore, 1 other semaphore, 1 mutex.

## Code shape

The file is split into two clearly separated halves:

1. **Core synchronization logic** (top): `barber()`, `customer()`, `main()`.
   This is the only part that matters for explaining the algorithm.
2. **Visualization / I/O** (bottom): colors, timestamps, log files, and
   spawning the 3 terminal windows. None of this affects correctness —
   delete it and the simulation still works, just silently.

## Walkthrough

**`barber()`** loops forever:
1. Logs that it's asleep, then blocks on `sem_wait(waiting_manager)` —
   this is the actual "sleep": zero CPU used until a customer posts.
2. On waking, locks `chair_lock` and checks: is the room empty *and* is
   the shop closed? If so, this wake-up was the shutdown signal, not a
   real customer — break out and go home.
3. Otherwise, decrements `chairs_used` (one customer is leaving the
   waiting room to sit in the barber chair), unlocks, and posts
   `barber_ready` so that specific customer knows their turn began.
4. Sleeps 2 seconds to simulate the haircut, then loops back.

**`customer()`**:
1. Locks `chair_lock`, checks if `chairs_used < CHAIRS`.
2. If yes: takes a chair (`chairs_used++`), unlocks, posts
   `waiting_manager` (wakes the barber or just increments the count if
   he's busy), then blocks on `sem_wait(barber_ready)` until it's
   specifically their turn.
3. If no: unlocks and leaves immediately — the room is full.

## The two required demo cases

**Simultaneous arrivals.** `main()` creates customer 1 and customer 2's
threads back-to-back with zero delay, so both race each other into
`customer()` at essentially the same instant. Since the entire
"check-and-increment" sequence on `chairs_used` runs while holding
`chair_lock`, only one of them can be inside that block at a time — the
other blocks on `pthread_mutex_lock()` until the first is done. The
mutex picks a winner order (decided by the OS scheduler, not the code),
but no update is ever lost and both customers still end up correctly
seated. This is the **"one process superseding another"** case: nothing
breaks, the race is just resolved safely and deterministically in terms
of *outcome* (both get seats), even if *who logs first* is not
deterministic.

**Staggered arrival.** Customer 3 arrives 2 seconds later, once the
barber may already be mid-haircut, exercising the "customer arrives
while the barber is busy" path.

## Shutdown handshake

After all 3 customer threads are joined, `main()` sets `shop_open = 0`
and posts `waiting_manager` exactly once. The barber, who may currently
be asleep on `sem_wait(waiting_manager)`, wakes up one final time, sees
`chairs_used == 0 && !shop_open`, and exits its loop instead of sleeping
forever.

## Why named semaphores (`sem_open`), not `sem_init`

Plain unnamed POSIX semaphores (`sem_init`/`sem_destroy`) are deprecated
and, in testing, unreliable on macOS — they can misfire and wake threads
that were never actually posted to. Named semaphores via `sem_open` give
identical `sem_wait`/`sem_post` semantics but behave correctly. Each
case uses uniquely-named semaphores (`/sb_case1_waiting_manager`,
`/sb_case1_barber_ready`) so the two cases never collide if run
back-to-back.

## The output: 3 live terminal windows

Instead of one interleaved, confusing console, each conceptual channel
gets its own window, exactly like the classic Dining Philosophers demos:

- **Waiting Room** — a live ASCII box showing which of the 3 chairs are
  occupied (`[XX]`) or free (`[  ]`), redrawn every time `chairs_used`
  changes.
- **Barber Shop** — the barber's own narration: asleep / woke up /
  finished.
- **Transactions** — every customer arrival, seating, service start, and
  rejection, in order.

Each channel writes to its own log file with `fflush()` after every
line; a spawned terminal simply runs `tail -f` on that file, so updates
appear live as the simulation runs.
