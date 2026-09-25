# Sleeping Barber — Variant 2 (Two Barbers) — Explainer

## What's different from variant 1

Same shop, same 3 waiting-room chairs — but now there are **two
barbers** working simultaneously, sharing that one waiting room. Any
free barber can pick up the next waiting customer; there's no rule
assigning specific customers to specific barbers.

Read `../variant1_single_barber/explainer_v1.md` first if you haven't —
this doc only covers what's new.

## The synchronization tools used here

| Tool | Name in code | Starting value | Purpose |
|---|---|---|---|
| Counting semaphore | `customers_sem` | 0 | Any idle barber blocks here until a customer signals |
| Counting semaphore | `barber_ready_sem` | 0 | A customer blocks here until *some* barber is ready for them |
| Mutex (lock) | `seats_mutex` | unlocked | Protects `waiting_count`, now touched by 2 barber threads + every customer thread |

The key design decision: **both barbers share the exact same two
semaphores and the exact same mutex**. Nothing barber-specific exists
in the synchronization layer — only the logging tags each barber with
its own ID (1 or 2) so you can tell them apart in the terminal windows.

## Why sharing one semaphore between two barbers is correct

`customers_sem` is a *counting* semaphore — its value is just an
integer count of "how many wake-ups are pending". When a customer sits
down, they call `sem_post(customers_sem)`, incrementing that count by
one. Either barber thread (whichever calls `sem_wait()` first, decided
by the OS scheduler, not by the code) will consume that increment and
proceed. There's no way for a post to be "lost" or for two barbers to
both claim the same customer — each `sem_wait()` call atomically
consumes exactly one unit, guaranteed by the OS.

This means we get "whichever barber is free takes the next customer"
behavior for free, just by having both barber threads run the identical
loop against shared semaphores. No extra coordination code needed.

## The barber loop (per barber thread)

```c
void *barber_thread(void *arg) {
    int barber_id = *(int *)arg;
    while (1) {
        log("Barber %d is sleeping...", barber_id);
        sem_wait(customers_sem);            // blocks until someone posts

        lock(seats_mutex);
        if (waiting_count == 0 && !shop_open) { unlock(); break; }
        waiting_count--;                     // claim one waiting customer
        unlock(seats_mutex);

        sem_post(barber_ready_sem);          // release that one customer
        sleep(haircut_time);
    }
}
```

`NUM_BARBERS` (= 2) of these run concurrently, each with a different
`barber_id`. They never coordinate directly with each other — all their
coordination happens indirectly, through the shared semaphore and mutex.

## The shutdown handshake, with two barbers

This is the one place the barber count actually changes the code. In
variant 1, one final `sem_post(customers_sem)` was enough to wake the
single sleeping barber for shutdown. With two barbers, **one post would
only wake one of them** — the other could stay asleep forever, since
nothing else would ever post to `customers_sem` again.

The fix, in `generator_thread()`:

```c
for (int i = 0; i < NUM_BARBERS; i++)
    sem_post(customers_sem);
```

One shutdown post per barber. Each barber thread wakes exactly once for
shutdown, sees `waiting_count == 0 && !shop_open`, and exits cleanly.
This is the general pattern for any N-barber version: N shutdown posts
for N barber threads.

## Why not give each barber their own semaphore/waiting room?

That's a valid alternative design (and closer to some multi-server
queueing models), but it changes the problem: customers would need to
pick a specific barber's queue up front, and one barber could be idle
while another's queue is full even though a shared room would have
served everyone. Sharing one queue (as this program does) is simpler,
matches "any available barber serves the next customer" real-shop
behavior, and needs zero extra synchronization primitives beyond
variant 1's — which is why the "keep it dead simple" version looks
almost identical to the single-barber file.

## Files in this folder

- `sleeping_barber_v2.c` — the clean version, no narrating comments.
- `sleeping_barber_v2_commented.c` — identical logic, commented almost
  line by line.
- `run_v2.sh` — compiles and runs the program.
- `explainer_v2.md` — this file.

## Running it

```bash
./run_v2.sh
```

Try 10+ customers and watch the "Barber Status" window — you'll see
both `Barber 1` and `Barber 2` interleaving haircuts, and the waiting
room draining faster than in variant 1 since two barbers now empty it
at once.
