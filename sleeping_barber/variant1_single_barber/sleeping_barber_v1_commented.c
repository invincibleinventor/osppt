/*
 * Sleeping Barber Problem - Variant 1: ONE barber, 3 waiting chairs.
 *
 * This is the exact same program as sleeping_barber_v1.c, but with a
 * comment on almost every line explaining what it does and why. Read
 * this file side by side with explainer_v1.md for the full picture.
 */

#include <stdio.h>      // printf, fprintf, scanf, FILE
#include <stdlib.h>     // malloc, free, rand, srand
#include <string.h>     // strcat, used to build the chairs row
#include <stdarg.h>     // va_list, needed for our custom logger's "..." args
#include <unistd.h>     // sleep()
#include <pthread.h>    // threads and mutexes
#include <semaphore.h>  // sem_t, sem_open, sem_wait, sem_post
#include <fcntl.h>      // O_CREAT / O_EXCL flags for sem_open
#include <time.h>       // time(), localtime(), strftime() for timestamps

#define NUM_CHAIRS 3    // the classic problem's 3 waiting-room chairs

/* ANSI escape codes: these just color terminal text. Each string starts
 * a color and CLR_RESET puts the terminal back to normal. */
#define CLR_RESET   "\033[0m"
#define CLR_RED     "\033[1;31m"
#define CLR_GREEN   "\033[1;32m"
#define CLR_YELLOW  "\033[1;33m"
#define CLR_BLUE    "\033[1;34m"
#define CLR_CYAN    "\033[1;36m"
#define CLR_MAGENTA "\033[1;35m"

/* --- The three synchronization primitives the whole problem is about --- */

/* customers_sem: starts at 0. A customer sem_post()s this the moment they
 * sit down, so the sleeping barber has something to sem_wait() on. This
 * is what lets the barber "sleep" (block) instead of busy-checking a flag. */
sem_t *customers_sem;

/* barber_ready_sem: starts at 0. The barber sem_post()s this once he has
 * picked a customer off the waiting_count, so that specific customer's
 * sem_wait() unblocks and they know "my turn has started". */
sem_t *barber_ready_sem;

/* seats_mutex: a plain lock. Anything that reads or writes waiting_count
 * must hold this lock first, because both the barber thread and every
 * customer thread touch waiting_count concurrently. Without this lock,
 * two threads could read the same old value and corrupt the count
 * (a classic race condition). */
pthread_mutex_t seats_mutex;

/* Note on macOS: sem_init() (unnamed semaphores) is deprecated and, in
 * practice, unreliable on macOS - it can misfire and wake threads that
 * were never posted to. Named semaphores via sem_open() are the fix,
 * which is why these are pointers opened later with sem_open(), not
 * plain sem_t values initialized with sem_init(). */

int waiting_count = 0;   // how many customers are currently in a chair
int shop_open = 1;       // 0 once the last customer has been generated
int total_served = 0;    // stats: how many got a haircut
int total_left = 0;      // stats: how many walked away, chairs full
int num_customers = 10;  // how many customers this run will generate

/* Three separate log files, one per "channel". Each one gets its own
 * spawned Terminal window doing `tail -f` on it, which is how we get
 * the multi-window live view. */
FILE *log_events, *log_barber, *log_room;

/* Fills buf with the current wall-clock time as HH:MM:SS, so every log
 * line can be timestamped. */
void timestamp(char *buf, size_t n) {
    time_t t = time(NULL);
    struct tm *lt = localtime(&t);
    strftime(buf, n, "%H:%M:%S", lt);
}

/* A tiny printf-style logger: writes a colored, timestamped line to
 * whichever log file (f) is passed in, then flushes immediately. The
 * fflush is important - without it, C would buffer the output and the
 * `tail -f` windows would show nothing until the buffer filled up. */
void log_line(FILE *f, const char *color, const char *fmt, ...) {
    char ts[16];
    timestamp(ts, sizeof(ts));
    va_list args;             // holds the "..." variadic arguments
    va_start(args, fmt);      // start reading them after fmt
    fprintf(f, "%s[%s] ", color, ts);   // color code + timestamp
    vfprintf(f, fmt, args);             // the actual message, e.g. "Customer 3 arrives"
    fprintf(f, "%s\n", CLR_RESET);      // reset color, newline
    fflush(f);                          // push it to disk/pipe now
    va_end(args);
}

/* Draws the waiting room as three boxes, e.g. [C][C][ ], and logs it.
 * Called any time waiting_count changes, always while seats_mutex is
 * already held by the caller. */
void draw_waiting_room(void) {
    char row[128] = "";
    for (int i = 0; i < NUM_CHAIRS; i++) {
        if (i < waiting_count)
            strcat(row, CLR_YELLOW "[C]" CLR_RESET);  // occupied chair
        else
            strcat(row, "[ ]");                       // empty chair
    }
    log_line(log_room, CLR_CYAN, "Chairs: %s  (%d/%d occupied)", row, waiting_count, NUM_CHAIRS);
}

/* The barber's whole life cycle, run on its own thread. */
void *barber_thread(void *arg) {
    (void)arg;  // unused, this variant only ever has one barber
    while (1) {
        log_line(log_barber, CLR_BLUE, "Barber is sleeping (no customers)...");

        /* sem_wait blocks here until customers_sem > 0. This IS the
         * "sleeping" in Sleeping Barber - the OS puts this thread to
         * sleep, using zero CPU, until a customer posts. */
        sem_wait(customers_sem);

        pthread_mutex_lock(&seats_mutex);
        /* We were woken up, but was it a real customer, or the shutdown
         * signal (shop closing with nobody left waiting)? If nobody is
         * actually waiting and the shop is closed, this was a shutdown
         * wake-up - break out of the loop and go home. */
        if (waiting_count == 0 && !shop_open) {
            pthread_mutex_unlock(&seats_mutex);
            break;
        }
        waiting_count--;          // this customer is leaving the waiting room
        draw_waiting_room();      // reflect the new chair state in the log
        pthread_mutex_unlock(&seats_mutex);

        log_line(log_barber, CLR_GREEN, "Barber woke up and is cutting hair");

        /* Tell the specific customer who's now "up" that the barber is
         * ready for them. */
        sem_post(barber_ready_sem);

        sleep(1 + rand() % 3);    // simulate the haircut taking 1-3 seconds
        log_line(log_barber, CLR_MAGENTA, "Barber finished a haircut");
    }
    log_line(log_barber, CLR_RED, "Shop is closed, barber goes home");
    return NULL;
}

/* One customer's life cycle: arrive, try to sit, either get served or
 * leave. Runs on its own thread, one thread per customer. */
void *customer_thread(void *arg) {
    int id = *(int *)arg;  // the customer's number, passed in by the generator
    free(arg);             // we own this heap allocation, done reading it now

    pthread_mutex_lock(&seats_mutex);
    if (waiting_count < NUM_CHAIRS) {
        /* There's a free chair - take it. */
        waiting_count++;
        log_line(log_events, CLR_GREEN, "Customer %d takes a seat", id);
        draw_waiting_room();
        pthread_mutex_unlock(&seats_mutex);

        /* Wake the barber (or queue up if he's already awake and busy -
         * the semaphore just becomes >1, no wakeup is lost). */
        sem_post(customers_sem);

        /* Block here until THIS customer's turn comes - the barber will
         * sem_post(barber_ready_sem) once he has picked this seat off
         * waiting_count. */
        sem_wait(barber_ready_sem);

        log_line(log_events, CLR_YELLOW, "Customer %d is getting a haircut", id);
        pthread_mutex_lock(&seats_mutex);
        total_served++;
        pthread_mutex_unlock(&seats_mutex);
    } else {
        /* All 3 chairs are full - this customer turns around and leaves,
         * exactly like the classic problem describes. */
        pthread_mutex_unlock(&seats_mutex);
        log_line(log_events, CLR_RED, "Customer %d leaves, no chairs free", id);
        pthread_mutex_lock(&seats_mutex);
        total_left++;
        pthread_mutex_unlock(&seats_mutex);
    }
    return NULL;
}

/* Spawns num_customers customer threads at random intervals, then closes
 * the shop once they've all been dealt with (served or turned away). */
void *generator_thread(void *arg) {
    (void)arg;
    pthread_t customers[num_customers];  // one thread handle per customer

    for (int i = 0; i < num_customers; i++) {
        sleep(1 + rand() % 2);                  // random gap between arrivals
        int *id = malloc(sizeof(int));          // heap-allocate so the new
        *id = i + 1;                            // thread can own its own copy
        log_line(log_events, CLR_CYAN, "Customer %d arrives at the shop", *id);
        pthread_create(&customers[i], NULL, customer_thread, id);
    }
    /* Wait for every customer thread to finish (served or turned away)
     * before we consider closing the shop. */
    for (int i = 0; i < num_customers; i++)
        pthread_join(customers[i], NULL);

    pthread_mutex_lock(&seats_mutex);
    shop_open = 0;             // no more customers will ever arrive
    pthread_mutex_unlock(&seats_mutex);

    /* The barber might be asleep on sem_wait(customers_sem) right now
     * with nothing left to serve. This post wakes him one last time so
     * he can see shop_open == 0 and exit his loop instead of sleeping
     * forever. */
    sem_post(customers_sem);
    return NULL;
}

/* Opens a new macOS Terminal.app window that just runs `tail -f` on one
 * of our log files, giving that channel its own live-updating window. */
void spawn_terminal(const char *title, const char *logfile) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
        "osascript -e 'tell application \"Terminal\" to do script "
        "\"clear; echo \\\"== %s ==\\\"; tail -f %s\"' "
        "-e 'delay 0.3' "
        "-e 'tell application \"Terminal\" to set custom title of front window to \"%s\"' "
        "> /dev/null 2>&1 &",
        title, logfile, title);
    system(cmd);  // hand the AppleScript command off to the shell
}

/* Opens (truncating) all three log files fresh, so each run starts with
 * clean windows instead of appending to last run's output. */
void reset_logs(void) {
    log_events = fopen("logs/events.log", "w");
    log_barber = fopen("logs/barber.log", "w");
    log_room   = fopen("logs/waiting_room.log", "w");
}

void close_logs(void) {
    fclose(log_events);
    fclose(log_barber);
    fclose(log_room);
}

/* Sets everything up, runs one full simulation, tears everything down,
 * and prints the final stats. */
void run_simulation(void) {
    system("mkdir -p logs");  // make sure the logs/ folder exists
    reset_logs();

    /* sem_unlink first in case a previous crashed run left the named
     * semaphore behind - without this, sem_open with O_EXCL would fail
     * on the second run. */
    sem_unlink("/sleeping_barber_v1_customers");
    sem_unlink("/sleeping_barber_v1_barber_ready");
    customers_sem = sem_open("/sleeping_barber_v1_customers", O_CREAT | O_EXCL, 0644, 0);
    barber_ready_sem = sem_open("/sleeping_barber_v1_barber_ready", O_CREAT | O_EXCL, 0644, 0);
    pthread_mutex_init(&seats_mutex, NULL);

    waiting_count = 0;
    shop_open = 1;
    total_served = 0;
    total_left = 0;

    /* Pop open the three visualization windows and give macOS a moment
     * to actually open them before the fast-moving simulation starts. */
    spawn_terminal("Waiting Room", "logs/waiting_room.log");
    spawn_terminal("Barber Status", "logs/barber.log");
    spawn_terminal("Shop Events", "logs/events.log");
    sleep(1);

    pthread_t barber, generator;
    pthread_create(&barber, NULL, barber_thread, NULL);
    pthread_create(&generator, NULL, generator_thread, NULL);

    /* Wait for the customer stream to finish, then for the barber to
     * notice shop_open == 0 and shut down. */
    pthread_join(generator, NULL);
    pthread_join(barber, NULL);

    close_logs();
    sem_close(customers_sem);
    sem_close(barber_ready_sem);
    sem_unlink("/sleeping_barber_v1_customers");
    sem_unlink("/sleeping_barber_v1_barber_ready");
    pthread_mutex_destroy(&seats_mutex);

    printf(CLR_GREEN "\nSimulation finished.\n" CLR_RESET);
    printf("Customers served: %d\n", total_served);
    printf("Customers turned away: %d\n\n", total_left);
}

void print_banner(void) {
    printf(CLR_CYAN);
    printf("=====================================\n");
    printf("   SLEEPING BARBER - ONE BARBER\n");
    printf("=====================================\n");
    printf(CLR_RESET);
}

void print_about(void) {
    printf(CLR_YELLOW);
    printf("\nClassic Sleeping Barber Problem (single barber)\n");
    printf("- %d waiting chairs\n", NUM_CHAIRS);
    printf("- 1 mutex protects the shared waiting_count\n");
    printf("- sem customers_sem: barber sleeps until a customer signals\n");
    printf("- sem barber_ready_sem: customer waits until barber is ready\n");
    printf(CLR_RESET "\n");
}

/* The menu. Everything above is just function definitions - this is
 * where the program actually starts running. */
int main(void) {
    srand(time(NULL));  // seed randomness so runs differ each time
    print_banner();

    int choice;
    do {
        printf("1) Start simulation\n");
        printf("2) About this variant\n");
        printf("3) Exit\n");
        printf("Choice: ");
        if (scanf("%d", &choice) != 1) { choice = 3; break; }  // bad input -> quit safely

        if (choice == 1) {
            printf("How many customers? ");
            scanf("%d", &num_customers);
            if (num_customers < 1) num_customers = 10;  // guard against 0 or negative
            run_simulation();
        } else if (choice == 2) {
            print_about();
        }
    } while (choice != 3);

    printf("Goodbye.\n");
    return 0;
}
