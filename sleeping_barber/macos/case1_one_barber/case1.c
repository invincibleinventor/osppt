/*
 * Sleeping Barber - Case 1: one barber, 3 chairs, 6 customers.
 * Demonstrates: normal arrivals, a priority customer called ahead of an
 * earlier-arrived one, and two customers racing for the last free chair.
 * Sync: waiting_manager (sem) + one per-customer "turn" sem + chair_lock (mutex).
 * macOS: opens Terminal.app windows for Waiting Room / Barber Shop / Transactions.
 */

#include <pthread.h>
#include <semaphore.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>

#define CHAIRS        3
#define MAX_CUSTOMERS 64

typedef struct {
    int occupied;
    int id;
    int priority;     /* 0 = normal, 1 = called ahead of normal customers */
    sem_t *turn;       /* this occupant's personal "you're up" semaphore  */
} chair_t;

chair_t chairs[CHAIRS];
sem_t *waiting_manager;   /* customer -> barber: "someone is waiting" */
pthread_mutex_t chair_lock;

void show_chairs(void);
void log_room(const char *fmt, ...);
void log_barber(const char *fmt, ...);
void log_tx(const char *fmt, ...);
void setup_display(void);
void teardown_display(void);

/* ---------------- core synchronization logic ---------------- */

typedef struct {
    int id;
    int priority;
    int contested;   /* true only for the two customers racing for one chair */
} customer_args_t;

static void spawn_customer(pthread_t *t, void *(*fn)(void *), int id, int priority, int contested) {
    customer_args_t *a = malloc(sizeof(customer_args_t));
    a->id = id; a->priority = priority; a->contested = contested;
    pthread_create(t, NULL, fn, a);
}

void *customer(void *arg);

void *barber(void *arg) {
    (void)arg;
    while (1) {
        log_barber("Barber is asleep, no customers waiting");
        sem_wait(waiting_manager);

        pthread_mutex_lock(&chair_lock);
        int winner = -1;
        for (int i = 0; i < CHAIRS; i++)
            if (chairs[i].occupied &&
                (winner == -1 || chairs[i].priority > chairs[winner].priority))
                winner = i;

        if (winner == -1) {   /* nobody waiting: this was the shutdown post */
            pthread_mutex_unlock(&chair_lock);
            break;
        }

        for (int i = 0; i < CHAIRS; i++)
            if (i != winner && chairs[i].occupied && chairs[winner].priority > chairs[i].priority) {
                log_room("Customer %d is prioritized ahead of Customer %d", chairs[winner].id, chairs[i].id);
                log_tx("Customer %d is prioritized ahead of Customer %d", chairs[winner].id, chairs[i].id);
            }

        int called_id = chairs[winner].id;
        sem_t *called_turn = chairs[winner].turn;
        chairs[winner].occupied = 0;
        show_chairs();
        pthread_mutex_unlock(&chair_lock);

        log_barber("Barber calls Customer %d in for a haircut", called_id);
        sem_post(called_turn);
        sleep(2);
        log_barber("Barber finishes Customer %d's haircut", called_id);
    }
    log_barber("Shop closed, barber goes home");
    return NULL;
}

void *customer(void *arg) {
    customer_args_t *a = (customer_args_t *)arg;
    int id = a->id, priority = a->priority, contested = a->contested;
    free(a);

    char sem_name[32];
    snprintf(sem_name, sizeof(sem_name), "/sb_case1_turn_%d", id);
    sem_unlink(sem_name);
    sem_t *my_turn = sem_open(sem_name, O_CREAT | O_EXCL, 0644, 0);

    pthread_mutex_lock(&chair_lock);
    int slot = -1;
    for (int i = 0; i < CHAIRS; i++)
        if (!chairs[i].occupied) { slot = i; break; }

    if (slot == -1) {
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
    chairs[slot].turn = my_turn;
    show_chairs();
    pthread_mutex_unlock(&chair_lock);

    log_tx("Customer %d takes a seat in the waiting room", id);
    if (contested)
        log_room("Customer %d wins the race and takes the last chair", id);

    sem_post(waiting_manager);
    sem_wait(my_turn);
    log_tx("Customer %d moves to the barber chair", id);

    sem_close(my_turn);
    sem_unlink(sem_name);
    return NULL;
}

int main(void) {
    setup_display();

    pthread_mutex_init(&chair_lock, NULL);
    waiting_manager = sem_open("/sb_case1_waiting_manager", O_CREAT | O_EXCL, 0644, 0);

    pthread_t barber_t;
    pthread_create(&barber_t, NULL, barber, NULL);

    pthread_t customer_threads[MAX_CUSTOMERS];
    int customer_count = 0;
    int next_id = 1;
    int choice;

    do {
        printf("\n=== Sleeping Barber - Case 1 (1 barber, 3 chairs) ===\n");
        printf("1) A customer arrives\n");
        printf("2) A customer arrives and is prioritized ahead of those waiting\n");
        printf("3) Two customers arrive at the same time, racing for one seat\n");
        printf("4) Close the shop and exit\n");
        printf("Choose: ");
        if (scanf("%d", &choice) != 1) {
            while (getchar() != '\n') { }
            continue;
        }

        if (choice == 1 && customer_count < MAX_CUSTOMERS) {
            spawn_customer(&customer_threads[customer_count++], customer, next_id++, 0, 0);
        } else if (choice == 2 && customer_count < MAX_CUSTOMERS) {
            spawn_customer(&customer_threads[customer_count++], customer, next_id++, 1, 0);
        } else if (choice == 3 && customer_count + 1 < MAX_CUSTOMERS) {
            log_tx("Customer %d and Customer %d arrive at the same time", next_id, next_id + 1);
            spawn_customer(&customer_threads[customer_count++], customer, next_id++, 0, 1);
            spawn_customer(&customer_threads[customer_count++], customer, next_id++, 0, 1);
        } else if (choice != 4) {
            printf("Invalid choice.\n");
        }
    } while (choice != 4);

    for (int i = 0; i < customer_count; i++)
        pthread_join(customer_threads[i], NULL);

    sem_post(waiting_manager);   /* wake the barber one last time so it sees no one waiting and exits */
    pthread_join(barber_t, NULL);

    sem_close(waiting_manager);
    sem_unlink("/sb_case1_waiting_manager");
    pthread_mutex_destroy(&chair_lock);

    teardown_display();
    return 0;
}

/* ---------------- visualization / I/O (not the sync logic) ---------------- */

#include <string.h>
#include <stdarg.h>
#include <time.h>

#define CLR_RESET   "\033[0m"
#define CLR_RED     "\033[1;31m"
#define CLR_GREEN   "\033[1;32m"
#define CLR_YELLOW  "\033[1;33m"
#define CLR_CYAN    "\033[1;36m"
#define CLR_MAGENTA "\033[1;35m"

FILE *f_room, *f_barber, *f_tx;

static void stamp(char *buf, size_t n) {
    time_t t = time(NULL);
    strftime(buf, n, "%H:%M:%S", localtime(&t));
}

static void vlog(FILE *f, const char *color, const char *fmt, va_list args) {
    char ts[16]; stamp(ts, sizeof(ts));
    fprintf(f, "%s[%s] ", color, ts);
    vfprintf(f, fmt, args);
    fprintf(f, "%s\n", CLR_RESET);
    fflush(f);
}

void log_room(const char *fmt, ...) {
    va_list a; va_start(a, fmt); vlog(f_room, CLR_YELLOW, fmt, a); va_end(a);
}
void log_barber(const char *fmt, ...) {
    va_list a; va_start(a, fmt); vlog(f_barber, CLR_GREEN, fmt, a); va_end(a);
}
void log_tx(const char *fmt, ...) {
    va_list a; va_start(a, fmt); vlog(f_tx, CLR_CYAN, fmt, a); va_end(a);
}

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

static void spawn_terminal(const char *title, const char *logfile) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
        "osascript -e 'tell application \"Terminal\" to do script "
        "\"clear; tail -f %s\"' "
        "-e 'tell application \"Terminal\" to set custom title of front window to \"%s\"' "
        "> /dev/null 2>&1 &",
        logfile, title);
    system(cmd);
}

void setup_display(void) {
    char cwd[512];
    getcwd(cwd, sizeof(cwd));
    system("mkdir -p logs");

    /* Absolute paths: a freshly opened terminal window starts in the
     * user's home directory, not this program's working directory, so
     * a relative "logs/..." path fails there with "No such file". */
    char room_path[600], barber_path[600], tx_path[600];
    snprintf(room_path, sizeof(room_path), "%s/logs/waiting_room.log", cwd);
    snprintf(barber_path, sizeof(barber_path), "%s/logs/barber_shop.log", cwd);
    snprintf(tx_path, sizeof(tx_path), "%s/logs/transactions.log", cwd);

    f_room   = fopen(room_path, "w");
    f_barber = fopen(barber_path, "w");
    f_tx     = fopen(tx_path, "w");
    spawn_terminal("Waiting Room", room_path);
    spawn_terminal("Barber Shop", barber_path);
    spawn_terminal("Transactions", tx_path);
    sleep(1);
}

void teardown_display(void) {
    fclose(f_room); fclose(f_barber); fclose(f_tx);
}
