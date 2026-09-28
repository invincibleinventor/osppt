/*
 * Sleeping Barber - Case 1: one barber, 3 chairs, 6 customers.
 *
 * Same program as case1.c, commented line by line. This version goes
 * beyond the textbook problem to also demonstrate:
 *   - a priority customer being called in ahead of one who arrived earlier
 *   - two customers racing for the very last free chair
 * The file is split into two halves on purpose:
 *   1) core synchronization logic - kept as small as the extra scenarios allow
 *   2) visualization / I/O - printing, colors, spawning terminals
 * When presenting, only the first half matters for explaining the
 * algorithm; the second half is just "how we made it pretty on screen".
 */

#include <pthread.h>    // threads, mutex
#include <semaphore.h>  // sem_t, sem_open, sem_wait, sem_post
#include <fcntl.h>      // O_CREAT / O_EXCL for sem_open
#include <stdlib.h>     // malloc, free
#include <unistd.h>     // sleep(), usleep()
#include <stdio.h>      // snprintf, for building semaphore names

#define CHAIRS        3   // 3 waiting-room chairs, per the problem statement
#define MAX_CUSTOMERS 64  // upper bound on how many the menu loop can spawn in one run

/* Each physical chair remembers who's sitting in it right now, so the
 * barber can choose WHICH waiting customer to call next instead of just
 * "the next one in line" - that's what makes priority possible. */
typedef struct {
    int occupied;
    int id;
    int priority;     /* higher = called sooner; equal priority falls back to FCFS */
    int seq;          /* arrival order, used to break priority ties (FCFS) */
    sem_t *turn;       /* this occupant's personal "you're up" semaphore  */
} chair_t;

chair_t chairs[CHAIRS];

/* waiting_manager: starts at 0. A customer posts this the moment they
 * sit down. The barber blocks on sem_wait(waiting_manager) whenever the
 * shop is empty - this IS the "sleeping" in Sleeping Barber: the OS
 * puts the barber thread to sleep (0% CPU) until someone posts. */
sem_t *waiting_manager;

/* Assigned to a chair the instant its occupant sits down (under
 * chair_lock, so it's race-free). The barber uses it to break ties
 * between two equal-priority waiting customers - lower seq sat down
 * earlier, so it wins: this is what makes equal priority fall back to
 * plain FCFS instead of an arbitrary chair-index order. */
int arrival_seq = 0;

/* chair_lock: a plain mutex. Both the barber and every customer thread
 * read/write the chairs[] array, so any access to it must happen while
 * holding this lock - otherwise two threads could race and corrupt the
 * state (e.g. two customers both see the same "free" chair and both
 * sit in it, silently overwriting each other's data). */
pthread_mutex_t chair_lock;

/* Forward declarations for the display-layer functions defined at the
 * bottom of this file - the core logic above only calls them, it never
 * needs to know how they work. */
void show_chairs(void);
void log_room(const char *fmt, ...);
void log_barber(const char *fmt, ...);
void log_tx(const char *fmt, ...);
void setup_display(void);
void teardown_display(void);

/* ---------------- core synchronization logic ---------------- */

/* Bundles a customer's id/priority/contested-flag so it can be handed
 * to pthread_create as a single void* argument. "contested" is not a
 * sync primitive - it just tells customer() whether to print the extra
 * "wins/loses the race" line for this specific scripted scenario. */
typedef struct {
    int id;
    int priority;
    int contested;
} customer_args_t;

static void spawn_customer(pthread_t *t, void *(*fn)(void *), int id, int priority, int contested) {
    customer_args_t *a = malloc(sizeof(customer_args_t));
    a->id = id; a->priority = priority; a->contested = contested;
    pthread_create(t, NULL, fn, a);
}

void *customer(void *arg);   // barber() below calls nothing of customer's,
                              // but this keeps both functions visible together

/* The barber's entire life, run on one thread. */
void *barber(void *arg) {
    (void)arg;  // no per-thread data needed, only one barber in this case
    while (1) {
        log_barber("Barber is asleep, no customers waiting");

        /* Blocks here until waiting_manager > 0. This is the actual
         * "sleep" - zero CPU used while there's nothing to do. */
        sem_wait(waiting_manager);

        pthread_mutex_lock(&chair_lock);

        /* Instead of just taking "whoever is next", scan all CHAIRS
         * chairs and pick the occupied one with the HIGHEST priority.
         * Ties (equal priority) are broken by seq - whoever sat down
         * earliest wins - so equal-priority customers fall back to
         * true FCFS instead of an arbitrary chair-index order. */
        int winner = -1;
        for (int i = 0; i < CHAIRS; i++) {
            if (!chairs[i].occupied) continue;
            if (winner == -1
                || chairs[i].priority > chairs[winner].priority
                || (chairs[i].priority == chairs[winner].priority && chairs[i].seq < chairs[winner].seq))
                winner = i;
        }

        /* A customer only ever posts waiting_manager AFTER successfully
         * occupying a chair (see customer() below). So the only way we
         * can wake up here and find NO chair occupied is the one
         * deliberate extra post main() sends after every real customer
         * is done - i.e. this wake-up is the shutdown signal. */
        if (winner == -1) {
            pthread_mutex_unlock(&chair_lock);
            break;
        }

        /* If the chosen winner has higher priority than some other
         * still-waiting customer, that's a real "called ahead of"
         * event - announce it in both the Waiting Room and
         * Transactions windows before we clear the chair. */
        for (int i = 0; i < CHAIRS; i++)
            if (i != winner && chairs[i].occupied && chairs[winner].priority > chairs[i].priority) {
                log_room("Customer %d is prioritized ahead of Customer %d", chairs[winner].id, chairs[i].id);
                log_tx("Customer %d is prioritized ahead of Customer %d", chairs[winner].id, chairs[i].id);
            }

        /* Copy out what we need locally BEFORE unlocking. Once we
         * unlock, a new customer could immediately reuse this same
         * chair slot and overwrite chairs[winner].id / .turn - but
         * called_id and called_turn are our own local copies, so that
         * overwrite can never affect the customer we're about to call. */
        int called_id = chairs[winner].id;
        sem_t *called_turn = chairs[winner].turn;
        chairs[winner].occupied = 0;   // this customer is leaving the waiting room
        show_chairs();                  // reflect the new chair state visually
        pthread_mutex_unlock(&chair_lock);

        log_barber("Barber calls Customer %d in for a haircut", called_id);
        sem_post(called_turn);   // wake up exactly this customer, no one else
        sleep(2);                 // simulate the haircut taking 2 seconds
        log_barber("Barber finishes Customer %d's haircut", called_id);
    }
    log_barber("Shop closed, barber goes home");
    return NULL;
}

/* One customer's life, run on its own thread - one thread per customer. */
void *customer(void *arg) {
    customer_args_t *a = (customer_args_t *)arg;
    int id = a->id, priority = a->priority, contested = a->contested;
    free(a);

    /* Each customer gets their OWN named semaphore, unique to their id.
     * This is what lets the barber call a SPECIFIC customer instead of
     * just "whoever is first in line" - a single shared semaphore can't
     * target one particular waiter, but a personal one can. Named
     * (sem_open), not sem_init: unnamed POSIX semaphores are deprecated
     * and unreliable on macOS. */
    char sem_name[32];
    snprintf(sem_name, sizeof(sem_name), "/sb_case1_turn_%d", id);
    sem_unlink(sem_name);   // clean up if a previous run crashed and left this behind
    sem_t *my_turn = sem_open(sem_name, O_CREAT | O_EXCL, 0644, 0);

    pthread_mutex_lock(&chair_lock);

    /* Look for the first free chair. This whole search-and-claim runs
     * while holding chair_lock, so even if two customers call this
     * function at the exact same instant, only one of them can be
     * inside this block at a time - the other blocks on
     * pthread_mutex_lock until the first is done touching chairs[].
     * That's how "customer 5 and 6 arrive together for the last chair"
     * is resolved safely: the mutex picks a winner order, and whoever
     * the OS scheduler lets through first claims the chair - no chair
     * is ever double-booked. */
    int slot = -1;
    for (int i = 0; i < CHAIRS; i++)
        if (!chairs[i].occupied) { slot = i; break; }

    if (slot == -1) {
        /* No free chair - this customer leaves immediately, exactly
         * like the classic problem describes. */
        pthread_mutex_unlock(&chair_lock);
        log_tx("Customer %d finds no free chair and leaves", id);
        if (contested)
            log_room("Customer %d loses the race for the last chair", id);
        sem_close(my_turn);
        sem_unlink(sem_name);
        return NULL;
    }

    chairs[slot].occupied = 1;
    chairs[slot].id = id;
    chairs[slot].priority = priority;
    chairs[slot].seq = arrival_seq++;   /* stamps this seat with its arrival order, for FCFS tie-breaks */
    chairs[slot].turn = my_turn;
    show_chairs();
    pthread_mutex_unlock(&chair_lock);

    log_tx("Customer %d takes a seat in the waiting room", id);
    if (contested)
        log_room("Customer %d wins the race and takes the last chair", id);

    sem_post(waiting_manager);   // tell the barber "someone is waiting"
    sem_wait(my_turn);           // block until the barber calls THIS customer specifically
    log_tx("Customer %d moves to the barber chair", id);

    sem_close(my_turn);
    sem_unlink(sem_name);
    return NULL;
}

// Returns 1 with *out set on success, 0 after invalid non-numeric input
// (already drained to the next line), or -1 on EOF. The EOF case matters
// because the drain loop below reads until '\n', and at EOF getchar()
// keeps returning EOF forever, never '\n' - without checking for it here
// a closed stdin (e.g. piped input running out) would spin the caller
// forever instead of closing the shop.
static int read_int(int *out) {
    if (scanf("%d", out) == 1) return 1;
    int c;
    while ((c = getchar()) != '\n' && c != EOF) { }
    return (c == EOF) ? -1 : 0;
}

int main(void) {
    setup_display();  // open log files + pop open the 3 terminal windows

    pthread_mutex_init(&chair_lock, NULL);
    /* Unlink first: if a previous run was killed before it could
     * sem_unlink this name, O_EXCL below would fail on a stale
     * leftover semaphore (wrong count) instead of creating a fresh
     * one at 0, making the barber think a customer already posted. */
    sem_unlink("/sb_case1_waiting_manager");
    waiting_manager = sem_open("/sb_case1_waiting_manager", O_CREAT | O_EXCL, 0644, 0);

    pthread_t barber_t;
    pthread_create(&barber_t, NULL, barber, NULL);

    /* Every customer this run spawns gets tracked here so main() can
     * join them all before shutting the barber down. next_id just
     * counts up so every customer gets a distinct, increasing number
     * regardless of which menu option created them. */
    pthread_t customer_threads[MAX_CUSTOMERS];
    int customer_count = 0;
    int next_id = 1;

    printf("\n=== Sleeping Barber - Case 1 (1 barber, %d chairs) ===\n", CHAIRS);

    /* Instead of a fixed menu of canned scenarios, the presenter first
     * seeds the waiting room directly - e.g. entering CHAIRS-1 here
     * leaves exactly one vacancy, which the next round's arrivals can
     * then fight over. Priority is asked per customer since it's what
     * drives who the barber calls next. */
    int initial;
    do {
        printf("Customers already in the waiting room at open (0-%d): ", CHAIRS);
        int r = read_int(&initial);
        if (r == -1) { initial = 0; break; }   // stdin closed: open with an empty room
        if (r == 0) { initial = -1; }
    } while (initial < 0 || initial > CHAIRS);

    for (int i = 0; i < initial; i++) {
        int prio;
        printf("  Priority for customer %d (0 = normal, higher = served sooner): ", next_id);
        if (read_int(&prio) != 1) prio = 0;
        spawn_customer(&customer_threads[customer_count++], customer, next_id++, prio, 0);
    }

    /* Each round asks how many customers arrive together this time.
     * Zero delay between the spawn_customer() calls in a round of size
     * >1 is the point - those threads enter customer() at essentially
     * the same instant and genuinely race for chair_lock over whatever
     * chairs happen to be free right then, exactly like customers 5/6
     * in the original scripted version, except now the presenter
     * chooses when it happens instead of it being hardcoded. 0 closes
     * the shop. */
    int n;
    do {
        printf("\nCustomers arriving this round (0 to close the shop): ");
        int r = read_int(&n);
        if (r == -1) break;   // stdin closed: close the shop
        if (r == 0) continue;
        if (n < 0) { printf("Invalid input.\n"); continue; }
        if (n == 0) break;

        /* Ask every priority in this round BEFORE spawning any thread.
         * Interleaving "ask priority, spawn, ask priority, spawn" would
         * space the spawns out by however long typing takes, giving the
         * barber time to call and free each chair between them - which
         * would defeat "arrive at the same time" entirely. Collecting
         * priorities up front means the spawn loop below can fire all n
         * threads back-to-back with nothing between them, so they
         * actually race for chair_lock. */
        int round_ids[MAX_CUSTOMERS], round_prios[MAX_CUSTOMERS];
        int round_count = 0;
        for (int i = 0; i < n && customer_count + round_count < MAX_CUSTOMERS; i++) {
            int prio;
            printf("  Priority for customer %d (0 = normal, higher = served sooner): ", next_id + i);
            if (read_int(&prio) != 1) prio = 0;
            round_ids[round_count] = next_id + i;
            round_prios[round_count] = prio;
            round_count++;
        }
        next_id += round_count;

        /* Logged with the actual spawned count, not the requested n -
         * if MAX_CUSTOMERS capped round_count below n, the announcement
         * must match who really shows up, not who was asked for. */
        if (round_count > 1)
            log_tx("Customer %d through Customer %d arrive at the same time",
                   round_ids[0], round_ids[round_count - 1]);

        for (int i = 0; i < round_count; i++)
            spawn_customer(&customer_threads[customer_count++], customer, round_ids[i], round_prios[i], round_count > 1);
    } while (1);

    for (int i = 0; i < customer_count; i++)
        pthread_join(customer_threads[i], NULL);

    /* The barber might currently be asleep on sem_wait(waiting_manager)
     * with nothing left to serve. This post wakes him one final time;
     * he'll find every chair empty (winner == -1) and exit his loop
     * instead of sleeping forever. */
    sem_post(waiting_manager);
    pthread_join(barber_t, NULL);

    sem_close(waiting_manager);
    sem_unlink("/sb_case1_waiting_manager");
    pthread_mutex_destroy(&chair_lock);

    teardown_display();
    return 0;
}

/* ---------------- visualization / I/O (not the sync logic) ---------------- */
/* Everything below just formats and displays what already happened above.
 * None of this affects correctness - it could be deleted entirely and the
 * barber/customer logic would still be 100% correct, just silent. */

#include <string.h>
#include <stdarg.h>   // va_list, for our own printf-style loggers
#include <time.h>     // timestamps

#define CLR_RESET   "\033[0m"    // ANSI: reset terminal color
#define CLR_RED     "\033[1;31m"
#define CLR_GREEN   "\033[1;32m"
#define CLR_YELLOW  "\033[1;33m"
#define CLR_CYAN    "\033[1;36m"
#define CLR_MAGENTA "\033[1;35m"

/* Three separate log files - one per terminal window. */
FILE *f_room, *f_barber, *f_tx;

/* Fills buf with the current time as HH:MM:SS. */
static void stamp(char *buf, size_t n) {
    time_t t = time(NULL);
    strftime(buf, n, "%H:%M:%S", localtime(&t));
}

/* Shared helper behind all three loggers below: writes a colored,
 * timestamped line and flushes immediately (without the flush,
 * `tail -f` wouldn't show anything until C's output buffer filled up). */
static void vlog(FILE *f, const char *color, const char *fmt, va_list args) {
    char ts[16]; stamp(ts, sizeof(ts));
    fprintf(f, "%s[%s] ", color, ts);
    vfprintf(f, fmt, args);
    fprintf(f, "%s\n", CLR_RESET);
    fflush(f);
}

/* Waiting Room window: the chair diagram plus explicit priority/race
 * resolution announcements - this is where "mutex resolution" is shown. */
void log_room(const char *fmt, ...) {
    va_list a; va_start(a, fmt); vlog(f_room, CLR_YELLOW, fmt, a); va_end(a);
}
void log_barber(const char *fmt, ...) {
    va_list a; va_start(a, fmt); vlog(f_barber, CLR_GREEN, fmt, a); va_end(a);
}
void log_tx(const char *fmt, ...) {
    va_list a; va_start(a, fmt); vlog(f_tx, CLR_CYAN, fmt, a); va_end(a);
}

/* Draws the 3 chairs as a small bordered row: green [  ] free, red
 * [Cn] a normal customer occupying it, magenta [Pn] a priority
 * customer occupying it. */
void show_chairs(void) {
    fprintf(f_room, CLR_YELLOW "+---------- WAITING ROOM ----------+\n" CLR_RESET);
    fprintf(f_room, "| ");
    for (int i = 0; i < CHAIRS; i++) {
        if (!chairs[i].occupied)
            fprintf(f_room, CLR_GREEN "[  ]  " CLR_RESET);
        else if (chairs[i].priority)
            fprintf(f_room, CLR_MAGENTA "[P%d]  " CLR_RESET, chairs[i].id);
        else
            fprintf(f_room, CLR_RED "[C%d]  " CLR_RESET, chairs[i].id);
    }
    fprintf(f_room, "|\n" CLR_YELLOW "+-----------------------------------+\n\n" CLR_RESET);
    fflush(f_room);
}

/* Queries the main screen's pixel size via Finder's desktop window
 * bounds ({0, 0, width, height}), so quadrant tiling below adapts to
 * whatever display this is run on instead of a hardcoded resolution. */
static void get_screen_size(int *w, int *h) {
    *w = 1440; *h = 900;   /* fallback if the osascript query ever fails */
    /* JXA reads NSScreen directly - unlike "Finder ... window of desktop",
     * it doesn't depend on Finder having a desktop window object, which
     * fails intermittently with error -1728 on some setups. */
    FILE *p = popen("osascript -l JavaScript -e "
        "'ObjC.import(\"AppKit\"); var f = $.NSScreen.mainScreen.frame; "
        "f.size.width + \",\" + f.size.height' 2>/dev/null", "r");
    if (!p) return;
    int a, b;
    if (fscanf(p, "%d,%d", &a, &b) == 2) { *w = a; *h = b; }
    pclose(p);
}

/* Opens a new macOS Terminal.app window that just runs `tail -f` on one
 * log file, giving that channel its own live-updating window, then
 * moves it into one quarter of the screen (0=top-left, 1=top-right,
 * 2=bottom-left, 3=bottom-right) so all the spawned windows tile
 * automatically instead of stacking on top of each other. */
static void spawn_terminal(const char *title, const char *logfile, int quadrant) {
    static int screen_w = 0, screen_h = 0;
    if (screen_w == 0) get_screen_size(&screen_w, &screen_h);   /* query once, reuse for every window */
    int x1 = (quadrant % 2) * (screen_w / 2);
    int y1 = (quadrant / 2) * (screen_h / 2);
    int x2 = x1 + screen_w / 2;
    int y2 = y1 + screen_h / 2;

    char cmd[768];
    snprintf(cmd, sizeof(cmd),
        "osascript -e 'tell application \"Terminal\" to do script "
        "\"clear; tail -f %s\"' "
        "-e 'tell application \"Terminal\" to set custom title of front window to \"%s\"' "
        "-e 'tell application \"Terminal\" to set bounds of front window to {%d, %d, %d, %d}' "
        "> /dev/null 2>&1 &",
        logfile, title, x1, y1, x2, y2);
    system(cmd);  // hand the AppleScript command to the shell
}

void setup_display(void) {
    /* A freshly opened Terminal.app window starts in the user's home
     * directory, not this program's working directory - a relative
     * "logs/..." path handed to `tail -f` there would fail with
     * "No such file or directory" even though the file exists right
     * here. Resolving to an absolute path up front sidesteps that. */
    char cwd[512];
    getcwd(cwd, sizeof(cwd));
    system("mkdir -p logs");                        // make sure logs/ exists

    char room_path[600], barber_path[600], tx_path[600];
    snprintf(room_path, sizeof(room_path), "%s/logs/waiting_room.log", cwd);
    snprintf(barber_path, sizeof(barber_path), "%s/logs/barber_shop.log", cwd);
    snprintf(tx_path, sizeof(tx_path), "%s/logs/transactions.log", cwd);

    f_room   = fopen(room_path, "w");  // truncate for a fresh run
    f_barber = fopen(barber_path, "w");
    f_tx     = fopen(tx_path, "w");
    spawn_terminal("Waiting Room", room_path, 0);
    spawn_terminal("Barber Shop", barber_path, 1);
    spawn_terminal("Transactions", tx_path, 2);
    sleep(1);  // give macOS a moment to actually open the windows
}

void teardown_display(void) {
    fclose(f_room); fclose(f_barber); fclose(f_tx);
}
