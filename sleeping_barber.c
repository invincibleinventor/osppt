/* Sleeping Barber - OS synchronization demo.
 * Build: clang -pthread -o barber sleeping_barber.c
 * Run:   ./barber, then pick 1 (Case 1) or 2 (Case 2)
 */

#include <stdio.h>                                              // printf, sprintf, fgets
#include <stdlib.h>                                              // atoi
#include <unistd.h>                                              // usleep
#include <pthread.h>                                             // threads, mutexes, condition variables


/* =====================================================================
   SECTION 1: SMALL HELPERS
   A lock so threads don't print over each other, plus a sleep helper.
   ===================================================================== */

pthread_mutex_t printLock = PTHREAD_MUTEX_INITIALIZER;           // one global lock, shared by every thread's print

void say(char *who, char *msg) {                                 // thread-safe print: "WHO   message"
    pthread_mutex_lock(&printLock);                              // only one thread may print at a time
    printf("%-9s%s\n", who, msg);                                 // who, padded to 9 chars, then the message
    pthread_mutex_unlock(&printLock);                            // let the next thread print
}                                                                  // end say()

void waitMs(int ms) {                                             // sleep for ms milliseconds
    usleep(ms * 1000);                                            // usleep takes microseconds
}                                                                  // end waitMs()


/* =====================================================================
   SECTION 2: THE SEMAPHORE
   Just a counter, protected by a lock, with a condition variable so a
   thread can sleep until the counter goes above zero.
   ===================================================================== */

struct Sem {                                                      // one semaphore's worth of state
    int value;                                                    // how many "permits" are available
    pthread_mutex_t lock;                                         // protects value from simultaneous access
    pthread_cond_t cond;                                          // lets a thread sleep until value > 0
};                                                                 // end struct Sem

void semInit(struct Sem *s, int startValue) {                    // set up a semaphore with a starting count
    s->value = startValue;                                        // permits available at the start
    pthread_mutex_init(&s->lock, 0);                              // set up the lock with default settings
    pthread_cond_init(&s->cond, 0);                               // set up the condition variable
}                                                                  // end semInit()

void semWait(struct Sem *s) {                                     // take a permit, blocking if none are free
    pthread_mutex_lock(&s->lock);                                 // grab the lock before touching value
    while (s->value <= 0) pthread_cond_wait(&s->cond, &s->lock);  // sleep here until someone signals
    s->value = s->value - 1;                                      // take one permit
    pthread_mutex_unlock(&s->lock);                               // release the lock
}                                                                  // end semWait()

void semSignal(struct Sem *s) {                                   // release a permit, waking a sleeper if any
    pthread_mutex_lock(&s->lock);                                 // grab the lock before touching value
    s->value = s->value + 1;                                      // give one permit back
    pthread_cond_signal(&s->cond);                                // wake one waiting thread, if any
    pthread_mutex_unlock(&s->lock);                               // release the lock
}                                                                  // end semSignal()

int semTryWait(struct Sem *s) {                                   // like semWait, but never blocks
    pthread_mutex_lock(&s->lock);                                 // grab the lock before touching value
    int gotIt = 0;                                                 // assume failure until proven otherwise
    if (s->value > 0) {                                           // permit available right now?
        s->value = s->value - 1;                                  // take it
        gotIt = 1;                                                 // tell the caller it succeeded
    }                                                               // end if
    pthread_mutex_unlock(&s->lock);                               // release the lock
    return gotIt;                                                  // 1 = got a permit, 0 = did not
}                                                                  // end semTryWait()


/* =====================================================================
   SECTION 3: CASE 1 - one barber, three waiting chairs
   Covers: semaphore wake-up, race condition, mutex fix, aging.
   ===================================================================== */

int chairs;                                                       // how many waiting chairs are free
pthread_mutex_t chairsLock = PTHREAD_MUTEX_INITIALIZER;          // protects "chairs" in the safe version
struct Sem wakeupSem;                                              // barber blocks on this until a customer arrives

void *barber(void *arg) {                                         // the barber's thread: sleep, then cut hair once
    say("BARBER", "sleeping, no customers");                      // no one is here yet, so barber is idle
    semWait(&wakeupSem);                                          // block here until a customer signals
    say("BARBER", "woken up, cutting hair...");                   // a customer arrived and woke the barber
    waitMs(600);                                                   // simulate time spent cutting hair
    say("BARBER", "done.");                                        // haircut finished
    return NULL;                                                   // thread has no result to return
}                                                                  // end barber()

void *seatCustomerUnsafe(void *arg) {                              // BUGGY version: check chairs, then sit, no lock
    char *name = arg;                                              // customer's name, passed in via pthread_create
    char msg[80];                                                   // scratch buffer to build printed messages
    int seen = chairs;                                              // read the current chair count (unprotected!)

    sprintf(msg, "%s sees %d chair(s) free", name, seen);          // build the "sees N chairs" message
    say("MANAGER", msg);                                            // print it
    waitMs(300);                                                    // gap here is what makes the race visible

    if (seen > 0) {                                                 // still using the OLD value read earlier
        chairs = seen - 1;                                          // write based on stale data -> can overwrite others
        sprintf(msg, "%s, sit down!", name);                       // tell this customer to sit
    } else {                                                        // no chairs were seen free
        sprintf(msg, "%s, sorry, full.", name);                    // turn this customer away
    }                                                                // end if/else
    say("MANAGER", msg);                                            // print the final outcome
    return NULL;                                                    // thread has no result to return
}                                                                  // end seatCustomerUnsafe()

void *seatCustomerSafe(void *arg) {                                // FIXED version: same logic, wrapped in a lock
    pthread_mutex_lock(&chairsLock);                                // only one customer can check+update chairs now
    seatCustomerUnsafe(arg);                                        // reuse the exact same check-then-seat logic
    pthread_mutex_unlock(&chairsLock);                              // release the lock for the next customer
    return NULL;                                                    // thread has no result to return
}                                                                  // end seatCustomerSafe()

void runCase1() {                                                   // runs all four Case 1 scenes in order
    printf("\n== CASE 1: ONE BARBER, THREE WAITING CHAIRS ==\n");  // scene banner

    printf("\n-- semaphore wake-up --\n");                         // sub-heading for scene 1
    chairs = 3;                                                     // 3 chairs available for this scene
    semInit(&wakeupSem, 0);                                         // starts at 0: barber must wait to be signaled
    pthread_t barberThread;                                         // handle for the barber's thread
    pthread_create(&barberThread, 0, barber, 0);                    // start the barber thread (it goes to sleep)
    waitMs(300);                                                    // let the barber actually reach semWait first
    say("MANAGER", "Customer 1 arrives, signal the barber");        // narrate the arrival
    semSignal(&wakeupSem);                                          // wake the barber up
    pthread_join(barberThread, 0);                                  // wait for the barber thread to finish

    printf("\n-- race condition: 2 customers, 1 chair, no lock --\n"); // sub-heading for scene 2
    chairs = 1;                                                     // only 1 chair, so a clash is guaranteed
    pthread_t r1, r2;                                                // handles for the two racing customer threads
    pthread_create(&r1, 0, seatCustomerUnsafe, "Customer 2");       // both start almost simultaneously
    pthread_create(&r2, 0, seatCustomerUnsafe, "Customer 3");       // second customer, same unsafe function
    pthread_join(r1, 0);                                            // wait for both threads to finish
    pthread_join(r2, 0);                                            // wait for both threads to finish
    printf("BUG: both may have been told to sit in the SAME chair.\n"); // point out the bug

    printf("\n-- mutex fix: same scenario, with a lock --\n");     // sub-heading for scene 3
    chairs = 1;                                                     // reset to 1 chair again
    pthread_t m1, m2;                                                // handles for the two locked customer threads
    pthread_create(&m1, 0, seatCustomerSafe, "Customer 4");         // this time each customer locks chairsLock
    pthread_create(&m2, 0, seatCustomerSafe, "Customer 5");         // second customer, same safe function
    pthread_join(m1, 0);                                            // wait for both threads to finish
    pthread_join(m2, 0);                                            // wait for both threads to finish
    printf("FIXED: only one customer is ever seated at a time.\n"); // confirm the fix worked

    printf("\n-- starvation and aging (lower number = higher priority) --\n"); // sub-heading for scene 4
    int priority = 3;                                               // 3 = lowest priority in this demo
    char msg[80];                                                    // scratch buffer for this scene's messages

    sprintf(msg, "arrives, priority %d (low)", priority);           // build the arrival message
    say("CUSTOMER", msg);                                            // print it
    say("BARBER", "Customer 3 (priority 2) arrives, served first"); // a higher-priority customer jumps the queue

    priority = priority - 1;                                        // aging: waiting customer's priority improves
    sprintf(msg, "skipped, aging: priority now %d", priority);      // build the aging message
    say("CUSTOMER", msg);                                            // print it
    say("BARBER", "Customer 4 (priority 2) arrives, served next");  // skipped again by another priority-2 arrival

    priority = priority - 1;                                        // aging again: priority keeps climbing
    sprintf(msg, "skipped again, aging: priority now %d", priority); // build the second aging message
    say("CUSTOMER", msg);                                            // print it
    say("BARBER", "Customer 6 (priority 2) arrives...");            // another new customer arrives

    sprintf(msg, "priority %d beats 2, serving the aged customer FIRST", priority); // now outranks new arrivals
    say("BARBER", msg);                                              // print the final outcome
    printf("Without aging, this customer would have waited forever.\n"); // punchline for the scene
}                                                                  // end runCase1()


/* =====================================================================
   SECTION 4: CASE 2 - shared state
   Two barbers, one comb, one scissors, and a combined "kit".
   ===================================================================== */

struct Sem toolSem, combSem, scissorsSem, kitSem;                  // one semaphore per shared resource
int stopFlag;                                                       // watchdog: tells looping threads to stop
int barber1Count, barber2Count;                                     // how many customers each barber has served


/* --- 4a: semaphore blocking on a single shared tool --- */

void *barber1UsesTool(void *arg) {                                  // Barber 1 grabs the tool first
    semWait(&toolSem);                                              // takes the only permit (starts at 1)
    say("BARBER1", "has the tool, cutting hair...");                // narrate that Barber 1 is working
    waitMs(900);                                                    // holds the tool for a while
    say("BARBER1", "done, releases the tool.");                     // narrate finishing up
    semSignal(&toolSem);                                            // frees the tool for Barber 2
    return NULL;                                                    // thread has no result to return
}                                                                  // end barber1UsesTool()

void *barber2UsesTool(void *arg) {                                  // Barber 2 tries right after Barber 1 has it
    say("BARBER2", "tries to get the tool...");                     // narrate the attempt
    if (!semTryWait(&toolSem)) {                                    // non-blocking check: is it free right now?
        say("BARBER2", "BLOCKED, tool is busy");                    // no -> announce that it will now block
        semWait(&toolSem);                                          // blocks here until Barber 1 signals
    }                                                                // end if
    say("BARBER2", "unblocked, serving customer...");               // narrate that it finally got the tool
    waitMs(400);                                                    // simulate time spent cutting hair
    semSignal(&toolSem);                                            // release the tool when finished
    say("BARBER2", "done.");                                        // narrate finishing up
    return NULL;                                                    // thread has no result to return
}                                                                  // end barber2UsesTool()


/* --- 4b: deadlock, each barber grabs tools in the opposite order --- */

void *barber1Deadlock(void *arg) {                                  // Barber 1: comb first, then scissors
    semWait(&combSem);                                              // holds the comb (mutual exclusion + hold)
    say("BARBER1", "picks up COMB, needs SCISSORS next");           // narrate what it's holding and wants
    waitMs(500);                                                    // gives Barber 2 time to grab scissors too

    while (!semTryWait(&scissorsSem)) {                             // keep polling instead of blocking forever
        if (stopFlag) {                                              // watchdog says "give up now"
            say("BARBER1", "gives up, releases COMB");              // narrate giving up
            semSignal(&combSem);                                    // release what it's holding (no preemption otherwise)
            return NULL;                                             // exit the thread early
        }                                                             // end if
        say("BARBER1", "BLOCKED, waiting for SCISSORS");            // narrate being stuck
        waitMs(500);                                                 // wait a bit before polling again
    }                                                                // end while
    semSignal(&scissorsSem);                                        // only reached if it actually got both tools
    semSignal(&combSem);                                            // release the comb too
    return NULL;                                                    // thread has no result to return
}                                                                  // end barber1Deadlock()

void *barber2Deadlock(void *arg) {                                  // Barber 2: scissors first, then comb (opposite!)
    semWait(&scissorsSem);                                          // this opposite order is what causes the deadlock
    say("BARBER2", "picks up SCISSORS, needs COMB next");           // narrate what it's holding and wants
    waitMs(500);                                                    // gives Barber 1 time to grab the comb too

    while (!semTryWait(&combSem)) {                                 // Barber 1 is holding comb, Barber 2 is holding scissors
        if (stopFlag) {                                              // circular wait: neither can ever proceed
            say("BARBER2", "gives up, releases SCISSORS");          // narrate giving up
            semSignal(&scissorsSem);                                // release what it's holding
            return NULL;                                             // exit the thread early
        }                                                             // end if
        say("BARBER2", "BLOCKED, waiting for COMB");                // narrate being stuck
        waitMs(500);                                                 // wait a bit before polling again
    }                                                                // end while
    semSignal(&combSem);                                            // only reached if it actually got both tools
    semSignal(&scissorsSem);                                        // release the scissors too
    return NULL;                                                    // thread has no result to return
}                                                                  // end barber2Deadlock()


/* --- 4c: fix 1, always take scissors then comb -> no deadlock, but starves --- */

void *barber1Ordered(void *arg) {                                    // both barbers now use the SAME acquire order
    char msg[80];                                                     // scratch buffer for this thread's messages
    while (!stopFlag) {                                               // keep serving customers until time runs out
        semWait(&scissorsSem);                                        // scissors first...
        semWait(&combSem);                                            // ...then comb - same order as Barber 2 now

        barber1Count = barber1Count + 1;                              // one more customer served
        sprintf(msg, "serves a customer (served: %d)", barber1Count); // build the status message
        say("BARBER1", msg);                                          // print it

        semSignal(&combSem);                                          // release both immediately after serving
        semSignal(&scissorsSem);                                      // release the scissors too
        waitMs(150);                                                   // Barber 1 is fast, so it re-grabs tools quickly
    }                                                                  // end while
    return NULL;                                                      // thread has no result to return
}                                                                  // end barber1Ordered()

void *barber2Ordered(void *arg) {                                     // identical logic, just slower
    char msg[80];                                                      // scratch buffer for this thread's messages
    while (!stopFlag) {                                                // keep serving customers until time runs out
        semWait(&scissorsSem);                                         // scissors first, same order as Barber 1
        semWait(&combSem);                                             // then comb

        barber2Count = barber2Count + 1;                               // one more customer served
        sprintf(msg, "serves a customer (served: %d)", barber2Count);  // build the status message
        say("BARBER2", msg);                                           // print it

        semSignal(&combSem);                                           // release both immediately after serving
        semSignal(&scissorsSem);                                       // release the scissors too
        waitMs(600);                                                    // slower, so it keeps losing the race for tools
    }                                                                   // end while
    return NULL;                                                       // thread has no result to return
}                                                                  // end barber2Ordered()


/* --- 4d: fix 2, bind both tools into one "kit" -> no deadlock, no starving --- */

void *barber1Kit(void *arg) {                                         // comb+scissors are now ONE resource: kitSem
    char msg[80];                                                      // scratch buffer for this thread's messages
    int i;                                                              // loop counter
    for (i = 0; i < 4; i++) {                                          // serve exactly 4 customers for this demo
        semWait(&kitSem);                                               // one wait gets the whole kit atomically

        barber1Count = barber1Count + 1;                                // one more customer served
        sprintf(msg, "serves with the full kit (served: %d)", barber1Count); // build the status message
        say("BARBER1", msg);                                            // print it

        semSignal(&kitSem);                                             // release the whole kit at once
        waitMs(250);                                                     // simulate time spent cutting hair
    }                                                                    // end for
    return NULL;                                                        // thread has no result to return
}                                                                  // end barber1Kit()

void *barber2Kit(void *arg) {                                          // same idea, same speed as Barber 1 this time
    char msg[80];                                                       // scratch buffer for this thread's messages
    int i;                                                               // loop counter
    for (i = 0; i < 4; i++) {                                           // serve exactly 4 customers for this demo
        semWait(&kitSem);                                                // one wait gets the whole kit atomically

        barber2Count = barber2Count + 1;                                 // one more customer served
        sprintf(msg, "serves with the full kit (served: %d)", barber2Count); // build the status message
        say("BARBER2", msg);                                             // print it

        semSignal(&kitSem);                                              // release the whole kit at once
        waitMs(250);                                                      // simulate time spent cutting hair
    }                                                                     // end for
    return NULL;                                                         // thread has no result to return
}                                                                  // end barber2Kit()


/* --- 4e: run all four Case 2 scenes in order --- */

void runCase2() {                                                       // runs all four Case 2 scenes in order
    printf("\n== CASE 2: TWO BARBERS, SHARED TOOLS ==\n");             // scene banner

    printf("\n-- semaphore blocking: one shared tool --\n");           // sub-heading for scene 1
    semInit(&toolSem, 1);                                               // 1 permit = 1 tool available
    pthread_t x1, x2;                                                    // handles for the two barber threads
    pthread_create(&x1, 0, barber1UsesTool, 0);                         // Barber 1 starts first and grabs the tool
    waitMs(200);                                                        // small head start so Barber 1 gets there first
    pthread_create(&x2, 0, barber2UsesTool, 0);                         // Barber 2 starts and finds it busy
    pthread_join(x1, 0);                                                 // wait for Barber 1 to finish
    pthread_join(x2, 0);                                                 // wait for Barber 2 to finish

    printf("\n-- deadlock: comb + scissors, opposite order --\n");     // sub-heading for scene 2
    semInit(&combSem, 1);                                               // 1 comb...
    semInit(&scissorsSem, 1);                                           // ...1 scissors
    stopFlag = 0;                                                        // watchdog off: let them actually deadlock first
    pthread_t d1, d2;                                                    // handles for the two deadlocking threads
    pthread_create(&d1, 0, barber1Deadlock, 0);                         // comb -> scissors
    pthread_create(&d2, 0, barber2Deadlock, 0);                         // scissors -> comb (circular wait forms)
    waitMs(2500);                                                       // let the deadlock sit for a bit, visibly stuck
    printf("DEADLOCK: mutual exclusion, hold-and-wait, no preemption, circular wait.\n"); // name the conditions
    stopFlag = 1;                                                        // now tell both threads to give up and unwind
    pthread_join(d1, 0);                                                 // wait for Barber 1 to unwind
    pthread_join(d2, 0);                                                 // wait for Barber 2 to unwind
    printf("Recovered: both barbers released their tools.\n");         // confirm recovery

    printf("\n-- fix 1: resource ordering (scissors, then comb), causes starvation --\n"); // sub-heading for scene 3
    semInit(&combSem, 1);                                               // fresh semaphores for this scene
    semInit(&scissorsSem, 1);                                           // fresh semaphores for this scene
    stopFlag = 0;                                                        // let both barbers run for a fixed time window
    barber1Count = 0;                                                    // reset both counters before the race starts
    barber2Count = 0;                                                    // reset both counters before the race starts
    pthread_t o1, o2;                                                    // handles for the two ordered-access threads
    pthread_create(&o1, 0, barber1Ordered, 0);                          // fast barber
    pthread_create(&o2, 0, barber2Ordered, 0);                          // slow barber, same tool-acquire order
    waitMs(2000);                                                       // both threads race for tools during this window
    stopFlag = 1;                                                        // time's up, stop both loops
    pthread_join(o1, 0);                                                 // wait for Barber 1's loop to exit
    pthread_join(o2, 0);                                                 // wait for Barber 2's loop to exit
    printf("No deadlock, but Barber1 (%d served) starved Barber2 (%d served).\n", barber1Count, barber2Count); // report the skew

    printf("\n-- fix 2: bind tools into one resource, fair --\n");     // sub-heading for scene 4
    semInit(&kitSem, 1);                                                // single combined resource, 1 permit
    barber1Count = 0;                                                    // reset counters again for this scene
    barber2Count = 0;                                                    // reset counters again for this scene
    pthread_t f1, f2;                                                    // handles for the two kit-based threads
    pthread_create(&f1, 0, barber1Kit, 0);                              // both barbers now use the exact same resource
    pthread_create(&f2, 0, barber2Kit, 0);                              // second barber, same kit-based function
    pthread_join(f1, 0);                                                 // both run to completion (4 customers each)
    pthread_join(f2, 0);                                                 // wait for Barber 2 to finish too
    printf("No deadlock AND no starvation: %d vs %d, roughly equal.\n", barber1Count, barber2Count); // report the fair result
}                                                                  // end runCase2()


/* =====================================================================
   SECTION 5: MENU
   ===================================================================== */

int main() {                                                            // program entry point
    while (1) {                                                         // loop until the user chooses to exit
        printf("\nSLEEPING BARBER DEMO\n1) Case 1  2) Case 2  0) Exit\n> "); // show the menu
        char buf[16];                                                    // buffer to hold the typed line
        if (!fgets(buf, sizeof(buf), stdin)) break;                     // stop on EOF (e.g. piped input runs out)

        int choice = atoi(buf);                                         // turn the typed text into a number
        if (choice == 1) runCase1();                                    // run all of Case 1's scenes
        else if (choice == 2) runCase2();                               // run all of Case 2's scenes
        else if (choice == 0) break;                                    // exit the loop
    }                                                                    // end while
    return 0;                                                            // program finished successfully
}                                                                  // end main()
