/*
 * Sleeping Barber Problem - Variant 2: TWO barbers, 3 waiting chairs.
 *
 * This is the exact same program as sleeping_barber_v2.c, but with a
 * comment on almost every line. Read alongside explainer_v2.md.
 *
 * This variant reuses the exact same synchronization design as variant 1
 * (2 semaphores + 1 mutex) - the only real change is that NUM_BARBERS
 * threads now run the barber loop instead of just one, and each barber
 * thread carries its own numeric ID for logging. See explainer_v2.md for
 * why this still works correctly with multiple barbers sharing one queue.
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

#define NUM_CHAIRS 3    // still just 3 waiting-room chairs, shared by both barbers
#define NUM_BARBERS 2   // this is the only structural difference from variant 1

/* ANSI escape codes: these just color terminal text. */
#define CLR_RESET   "\033[0m"
#define CLR_RED     "\033[1;31m"
#define CLR_GREEN   "\033[1;32m"
#define CLR_YELLOW  "\033[1;33m"
#define CLR_BLUE    "\033[1;34m"
#define CLR_CYAN    "\033[1;36m"
#define CLR_MAGENTA "\033[1;35m"

/* --- The three synchronization primitives, shared by both barbers --- */

/* customers_sem: starts at 0. Every customer who sits down posts once.
 * Because it's a COUNTING semaphore, it doesn't matter which of the two
 * barber threads consumes any given post - whichever barber is free and
 * calls sem_wait() first gets it. That's exactly the "either idle barber
 * can take the next customer" behavior we want. */
sem_t *customers_sem;

/* barber_ready_sem: starts at 0. Whichever barber just claimed a seat
 * posts once, and that wakes up the one customer who is currently
 * sem_wait()-ing for their turn. */
sem_t *barber_ready_sem;

/* seats_mutex: protects waiting_count, which is now touched by TWO
 * barber threads plus every customer thread. This lock is even more
 * important here than in variant 1, since we have one more thread that
 * could race on the same counter. */
pthread_mutex_t seats_mutex;

/* Note on macOS: sem_init() (unnamed semaphores) is deprecated and, in
 * practice, unreliable on macOS - it can misfire and wake threads that
 * were never posted to. Named semaphores via sem_open() are the fix. */

int waiting_count = 0;   // how many customers are currently in a chair
int shop_open = 1;       // 0 once the last customer has been generated
int total_served = 0;    // stats: how many got a haircut
int total_left = 0;      // stats: how many walked away, chairs full
int num_customers = 10;  // how many customers this run will generate

/* Three separate log files, one per "channel", each tailed by its own
 * spawned Terminal window. */
FILE *log_events, *log_barber, *log_room;

/* Fills buf with the current wall-clock time as HH:MM:SS. */
void timestamp(char *buf, size_t n) {
    time_t t = time(NULL);
    struct tm *lt = localtime(&t);
    strftime(buf, n, "%H:%M:%S", lt);
}

/* A tiny printf-style logger: writes a colored, timestamped line to the
 * given log file and flushes immediately, so `tail -f` windows update
 * in real time instead of waiting on C's output buffering. */
void log_line(FILE *f, const char *color, const char *fmt, ...) {
    char ts[16];
    timestamp(ts, sizeof(ts));
    va_list args;
    va_start(args, fmt);
    fprintf(f, "%s[%s] ", color, ts);
    vfprintf(f, fmt, args);
    fprintf(f, "%s\n", CLR_RESET);
    fflush(f);
    va_end(args);
}

/* Draws the waiting room as three boxes, e.g. [C][C][ ]. Always called
 * with seats_mutex already held. */
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

/* One barber's life cycle. NUM_BARBERS threads run this same function,
 * each with a different barber_id, all sharing the same two semaphores
 * and the same mutex - that sharing is what makes them cooperate on one
 * queue instead of each needing their own private waiting room. */
void *barber_thread(void *arg) {
    int barber_id = *(int *)arg;  // which barber this thread represents (1 or 2)
    free(arg);                    // done reading the heap id, free it

    while (1) {
        log_line(log_barber, CLR_BLUE, "Barber %d is sleeping (no customers)...", barber_id);

        /* Both barber threads block here independently. Whichever one
         * gets woken by the next sem_post() is essentially a race the
         * OS scheduler decides - there's no guarantee barber 1 goes
         * before barber 2. That's fine: real barbers don't take turns
         * either, whoever's free grabs the next customer. */
        sem_wait(customers_sem);

        pthread_mutex_lock(&seats_mutex);
        /* Same shutdown check as variant 1: if there's truly nobody
         * waiting and the shop is closed, this wake-up was one of the
         * NUM_BARBERS shutdown posts, not a real customer - go home. */
        if (waiting_count == 0 && !shop_open) {
            pthread_mutex_unlock(&seats_mutex);
            break;
        }
        waiting_count--;          // this barber has claimed one waiting customer
        draw_waiting_room();
        pthread_mutex_unlock(&seats_mutex);

        log_line(log_barber, CLR_GREEN, "Barber %d woke up and is cutting hair", barber_id);
        sem_post(barber_ready_sem);   // release the one customer who's now "up"
        sleep(1 + rand() % 3);        // simulate the haircut taking 1-3 seconds
        log_line(log_barber, CLR_MAGENTA, "Barber %d finished a haircut", barber_id);
    }
    log_line(log_barber, CLR_RED, "Shop is closed, barber %d goes home", barber_id);
    return NULL;
}

/* One customer's life cycle - identical logic to variant 1. From a
 * customer's point of view it doesn't matter how many barbers there
 * are; they just sit down, signal, and wait for whichever barber shows
 * up to signal back. */
void *customer_thread(void *arg) {
    int id = *(int *)arg;
    free(arg);

    pthread_mutex_lock(&seats_mutex);
    if (waiting_count < NUM_CHAIRS) {
        waiting_count++;
        log_line(log_events, CLR_GREEN, "Customer %d takes a seat", id);
        draw_waiting_room();
        pthread_mutex_unlock(&seats_mutex);

        sem_post(customers_sem);      // wake up whichever barber is free
        sem_wait(barber_ready_sem);   // wait for that barber to claim me specifically

        log_line(log_events, CLR_YELLOW, "Customer %d is getting a haircut", id);
        pthread_mutex_lock(&seats_mutex);
        total_served++;
        pthread_mutex_unlock(&seats_mutex);
    } else {
        /* Both barbers are busy and all 3 chairs are full - leave. */
        pthread_mutex_unlock(&seats_mutex);
        log_line(log_events, CLR_RED, "Customer %d leaves, no chairs free", id);
        pthread_mutex_lock(&seats_mutex);
        total_left++;
        pthread_mutex_unlock(&seats_mutex);
    }
    return NULL;
}

/* Spawns num_customers customer threads at random intervals, then signals
 * shutdown to BOTH barbers once every customer has been dealt with. */
void *generator_thread(void *arg) {
    (void)arg;
    pthread_t customers[num_customers];

    for (int i = 0; i < num_customers; i++) {
        sleep(1 + rand() % 2);              // random gap between arrivals
        int *id = malloc(sizeof(int));
        *id = i + 1;
        log_line(log_events, CLR_CYAN, "Customer %d arrives at the shop", *id);
        pthread_create(&customers[i], NULL, customer_thread, id);
    }
    for (int i = 0; i < num_customers; i++)
        pthread_join(customers[i], NULL);

    pthread_mutex_lock(&seats_mutex);
    shop_open = 0;
    pthread_mutex_unlock(&seats_mutex);

    /* With NUM_BARBERS barber threads potentially asleep on
     * sem_wait(customers_sem), we need exactly NUM_BARBERS extra posts
     * so each one individually wakes up, sees shop_open == 0 with
     * nothing waiting, and exits. One post would only wake one barber -
     * the other would sleep forever. */
    for (int i = 0; i < NUM_BARBERS; i++)
        sem_post(customers_sem);
    return NULL;
}

/* Opens a new macOS Terminal.app window that runs `tail -f` on one of
 * our log files. */
void spawn_terminal(const char *title, const char *logfile) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
        "osascript -e 'tell application \"Terminal\" to do script "
        "\"clear; echo \\\"== %s ==\\\"; tail -f %s\"' "
        "-e 'delay 0.3' "
        "-e 'tell application \"Terminal\" to set custom title of front window to \"%s\"' "
        "> /dev/null 2>&1 &",
        title, logfile, title);
    system(cmd);
}

/* Opens (truncating) all three log files fresh for this run. */
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

/* Sets everything up, runs one full simulation with NUM_BARBERS barber
 * threads, tears everything down, and prints the final stats. */
void run_simulation(void) {
    system("mkdir -p logs");
    reset_logs();

    /* sem_unlink first in case a previous crashed run left the named
     * semaphore behind. */
    sem_unlink("/sleeping_barber_v2_customers");
    sem_unlink("/sleeping_barber_v2_barber_ready");
    customers_sem = sem_open("/sleeping_barber_v2_customers", O_CREAT | O_EXCL, 0644, 0);
    barber_ready_sem = sem_open("/sleeping_barber_v2_barber_ready", O_CREAT | O_EXCL, 0644, 0);
    pthread_mutex_init(&seats_mutex, NULL);

    waiting_count = 0;
    shop_open = 1;
    total_served = 0;
    total_left = 0;

    spawn_terminal("Waiting Room", "logs/waiting_room.log");
    spawn_terminal("Barber Status", "logs/barber.log");
    spawn_terminal("Shop Events", "logs/events.log");
    sleep(1);

    pthread_t barbers[NUM_BARBERS], generator;
    for (int i = 0; i < NUM_BARBERS; i++) {
        int *id = malloc(sizeof(int));   // each barber thread gets its own numbered id
        *id = i + 1;
        pthread_create(&barbers[i], NULL, barber_thread, id);
    }
    pthread_create(&generator, NULL, generator_thread, NULL);

    pthread_join(generator, NULL);
    for (int i = 0; i < NUM_BARBERS; i++)
        pthread_join(barbers[i], NULL);   // wait for both barbers to go home

    close_logs();
    sem_close(customers_sem);
    sem_close(barber_ready_sem);
    sem_unlink("/sleeping_barber_v2_customers");
    sem_unlink("/sleeping_barber_v2_barber_ready");
    pthread_mutex_destroy(&seats_mutex);

    printf(CLR_GREEN "\nSimulation finished.\n" CLR_RESET);
    printf("Customers served: %d\n", total_served);
    printf("Customers turned away: %d\n\n", total_left);
}

void print_banner(void) {
    printf(CLR_CYAN);
    printf("=====================================\n");
    printf("   SLEEPING BARBER - TWO BARBERS\n");
    printf("=====================================\n");
    printf(CLR_RESET);
}

void print_about(void) {
    printf(CLR_YELLOW);
    printf("\nClassic Sleeping Barber Problem (two barbers)\n");
    printf("- %d waiting chairs shared by both barbers\n", NUM_CHAIRS);
    printf("- 1 mutex protects the shared waiting_count\n");
    printf("- sem customers_sem: barbers sleep until a customer signals\n");
    printf("- sem barber_ready_sem: customer waits until a barber is ready\n");
    printf("- both barbers pull from the same chair queue, no ordering\n");
    printf("  guarantee between them - either free barber can take the\n");
    printf("  next customer\n");
    printf(CLR_RESET "\n");
}

/* The menu - identical structure to variant 1. */
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
