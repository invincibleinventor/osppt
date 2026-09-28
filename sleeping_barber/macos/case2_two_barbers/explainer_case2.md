# Sleeping Barber — Case 2 (2 barbers, 2 chairs, 1 shared toolset)

## What changes from case 1

Case 2 has **2 barbers, 2 waiting-room chairs**, and — the real twist —
**one shared set of tools** (a comb and a pair of scissors). A barber
needs both tools to cut hair, so the two barbers now contend for the
same two resources. This turns the exercise into a small deadlock/
starvation case study on top of the usual producer/consumer structure.

## The synchronization tools

| Name                        | Type              | Starts at | Purpose                                    |
|-----------------------------|-------------------|-----------|---------------------------------------------|
| `waiting_manager`           | named semaphore   | 0         | Customer → barbers: "someone is waiting"    |
| per-customer `turn`         | named semaphore   | 0         | A barber → that customer: "it's your turn"  |
| `chair_lock`                | mutex             | unlocked  | Protects `chairs[]`                          |
| `comb_lock`, `scissors_lock`| mutex (2)         | unlocked  | The two shared physical tools                |
| `queue_lock` + `queue_cond` | mutex + condvar   | —         | Guards a FIFO ticket queue in front of the tool locks |

## The deadlock risk, and how it's structurally avoided

If barber 1 grabbed the comb and barber 2 grabbed the scissors at the
same time, each would then block forever waiting for the other tool the
other barber is holding — classic deadlock. This code makes that
impossible by construction: **every barber acquires the tools in the
same fixed order, comb before scissors, always** (`acquire_tools()`).
Since neither barber ever holds scissors while waiting on the comb, a
circular wait can never form — one of the four necessary conditions for
deadlock (circular wait) is eliminated at the code level, not detected
and recovered from at runtime.

## The starvation risk, and how it's avoided

Fixed lock ordering alone doesn't guarantee fairness — with two mutexes
and unlucky scheduling, one barber could in principle keep re-acquiring
the tools before the other gets a chance. `acquire_tools()` puts a
**FIFO ticket queue** in front of the real locks:

1. On entry, a barber takes the next ticket number (`next_ticket++`)
   under `queue_lock` and blocks on `queue_cond` until `now_serving`
   reaches its ticket.
2. Only once it's actually its turn does it acquire `comb_lock` then
   `scissors_lock`.
3. `release_tools()` releases both mutexes, increments `now_serving`,
   and broadcasts `queue_cond` to wake whoever's ticket is now current.

Because tickets are handed out in arrival order and served strictly in
that order, whichever barber *asked first* is *served first*, no matter
how the OS schedules the two barber threads afterward. Neither barber
can be skipped indefinitely.

## Walkthrough

**`customer()`** — same shape as case 1 but with no priority: lock
`chair_lock`, take a free chair (`CHAIRS = 2`) or leave if full, post
`waiting_manager`, wait on its personal `turn` semaphore.

**`barber(id)`** — two threads (ids 1 and 2) run this concurrently:
1. Blocks on `sem_wait(waiting_manager)`.
2. Locks `chair_lock`, finds an occupied chair, frees it immediately
   (the customer has moved to a barber chair), posts that customer's
   `turn` semaphore.
3. Calls `acquire_tools(id)` — this is where a barber may block for a
   while if the other barber is currently cutting hair.
4. Cuts hair (`sleep(2)`), then `release_tools(id)`.
5. Loops back to sleep on `waiting_manager` again.

## Interactive menu: seeding + rounds

`main()` asks two things, same pattern as case 1 but without priority:

1. **Initial occupancy** — how many customers are already seated at open
   (0 to `CHAIRS`). Seeding `CHAIRS - 1` leaves exactly one vacancy.
2. **Rounds** — how many customers arrive together this round, spawned
   back-to-back with no delay:
   - `n == 1` — a plain arrival.
   - `n >= 2` — those customer threads race for whatever chairs are
     free at that instant (chair contention), and once seated, their
     assigned barbers will in turn race for the shared toolset (tool
     contention) — both races resolved safely by the same locking
     rules described above.
   - `n == 0` — closes the shop.

To specifically see one barber blocking on the other's tools, send
arrivals fast enough that both barbers pick up customers within the
same ~2-second haircut window — the Toolbox window will show one
barber's ticket waiting behind the other's.

## Shutdown handshake — why N posts for N barbers

With 2 barber threads potentially asleep on `sem_wait(waiting_manager)`,
a single shutdown post would only wake one of them. So `main()` posts
once per barber:

```c
for (int i = 0; i < BARBERS; i++)
    sem_post(waiting_manager);
```

Each barber wakes once, finds no occupied chairs, and exits.

## The output: 3 live terminal windows, auto-tiled (macOS only)

- **Barber Room** — each barber's own narration: asleep / calls
  customer / cuts hair / finishes.
- **Toolbox** — every ticket request, wait, and hand-off, plus a live
  "held by / now serving" box.
- **Waiting Room** — chair occupancy and arrivals/rejections.

`setup_display()` queries the screen size via `osascript` once and
positions each spawned Terminal.app window into its own quadrant
(top-left, top-right, bottom-left), so the three windows auto-tile on
launch instead of stacking. Each channel writes to its own log file with
`fflush()` after every line; the spawned terminal runs `tail -f` on it.
