# Sleeping Barber — Variant 1 (One Barber) — Explainer

## The classic problem

A barbershop has one barber, one barber chair, and a waiting room with a
fixed number of chairs (here, **3**). If the shop is empty, the barber
sleeps until a customer arrives. If a customer arrives and the barber is
asleep, they wake him up and get their hair cut. If the barber is busy
and there's a free waiting-room chair, the customer sits and waits. If
the waiting room is full, the customer leaves.

The problem is a classic OS textbook example because it looks trivial
but hides real race conditions: if the barber and a customer both touch
"is anyone waiting?" at the same instant without protection, you can get
a customer who sits down but is never noticed, or a barber who wakes up
to an empty room and goes back to sleep while someone is actually
sitting there.

## The synchronization tools used here

| Tool | Name in code | Starting value | Purpose |
|---|---|---|---|
| Counting semaphore | `customers_sem` | 0 | Lets the barber block ("sleep") until a customer signals |
| Counting semaphore | `barber_ready_sem` | 0 | Lets a specific customer block until the barber is ready for them |
| Mutex (lock) | `seats_mutex` | unlocked | Protects `waiting_count`, the one piece of data both sides touch |

That's the "1 semaphore, 1 mutex... classic numbers" shape of the
textbook solution, generalized to 2 semaphores because we model both
directions of the handshake (barber → customer and customer → barber)
explicitly, which makes the code easier to follow than folding both
into one semaphore.

**Why a semaphore and not just a `while` loop checking a flag?**
A `while` loop that keeps checking "is there a customer yet?" would spin
the CPU at 100% the whole time the barber is idle (called *busy-waiting*).
`sem_wait()` instead tells the OS "put this thread to sleep until someone
posts" — zero CPU used while idle, and the OS wakes the thread
immediately when a post happens. That's the actual mechanism behind the
word "sleeping" in Sleeping Barber.

**Why a mutex as well as semaphores?**
Semaphores handle *signaling* ("something happened, wake up"). The
mutex handles *mutual exclusion* — making sure only one thread edits
`waiting_count` at a time, so a read-modify-write like `waiting_count++`
can't be interleaved between two threads and lose an update.

## The walkthrough

1. **`main()`** shows a menu: start a simulation, view an explanation, or
   exit. Choosing "start" asks how many customers to simulate, then
   calls `run_simulation()`.

2. **`run_simulation()`** is the setup/teardown wrapper:
   - Creates a fresh `logs/` folder and truncates the three log files.
   - Opens the two named semaphores (see the macOS note below) and
     initializes the mutex.
   - Spawns 3 new macOS Terminal windows, each just running `tail -f`
     on one log file — this is the "spawn different terminal instances"
     requirement. One window shows the waiting room, one shows what the
     barber is doing, one shows a general event feed.
   - Starts the barber thread and the customer-generator thread, waits
     for both to finish, then cleans everything up and prints a
     summary.

3. **`barber_thread()`** is an infinite loop:
   - Logs "sleeping" and calls `sem_wait(customers_sem)`, which blocks
     until a customer posts.
   - Once woken, it locks the mutex, checks whether this was a real
     customer or the final shutdown signal (see below), decrements
     `waiting_count` if real, and unlocks.
   - Posts `barber_ready_sem` to let that customer know their turn has
     started, then `sleep()`s for 1–3 seconds to simulate cutting hair.

4. **`customer_thread()`** runs once per customer:
   - Locks the mutex, checks if `waiting_count < 3`.
     - If yes: takes a seat, unlocks, posts `customers_sem` (wakes the
       barber), then `sem_wait(barber_ready_sem)` (blocks until it's
       specifically their turn), then logs getting served.
     - If no: unlocks and logs leaving — the classic "shop is full"
       case.

5. **`generator_thread()`** creates one customer thread after a random
   delay of up to one second, waits for all of them to finish (served or turned away),
   then sets `shop_open = 0` and posts `customers_sem` one more time so
   the barber (who might be asleep waiting for a customer that will
   never come) wakes up, sees the shop is closed with nobody waiting,
   and exits its loop instead of sleeping forever.

## The shutdown handshake, in detail

This is the one subtle part of the whole program. The barber's loop
always calls `sem_wait(customers_sem)` first, then checks *why* it woke
up:

```c
sem_wait(customers_sem);
if (waiting_count == 0 && !shop_open) {
    break;  // this wasn't a real customer - shut down
}
```

Every real customer posts `customers_sem` exactly once, and the
generator posts it exactly one more time after the shop closes. That
guarantees the barber wakes up one final time with nothing left to do,
and can tell the difference between "a customer is here" and "time to
go home" using the same semaphore.

## Why named semaphores (`sem_open`), not `sem_init`

The standard textbook way to create a semaphore is:

```c
sem_t s;
sem_init(&s, 0, 0);
```

On macOS specifically, this "unnamed" form is deprecated and, in
testing, unreliable — the barber thread would wake up far more times
than any customer or shutdown signal ever posted, looping forever. The
fix used throughout this program is *named* semaphores:

```c
sem_t *s = sem_open("/some-unique-name", O_CREAT | O_EXCL, 0644, 0);
```

Functionally these behave exactly the same (`sem_wait`/`sem_post` work
identically); the only difference is how they're created and destroyed
(`sem_open`/`sem_close`/`sem_unlink` instead of `sem_init`/`sem_destroy`).
If you move this code to Linux, plain `sem_init` works fine and you
could switch back if you prefer — but named semaphores work everywhere,
so there was no reason to special-case it.

## Files in this folder

- `sleeping_barber_v1.c` — the clean version, no narrating comments.
- `sleeping_barber_v1_commented.c` — identical logic, commented almost
  line by line.
- `run_v1.sh` — compiles and runs the program.
- `explainer_v1.md` — this file.

## Running it

```bash
./run_v1.sh
```

Pick "Start simulation" from the menu and enter how many customers to
simulate (try 8–10 to see chairs fill up and customers get turned away).
Three Terminal windows will pop up automatically to visualize the
waiting room, the barber's status, and the overall event feed live.
