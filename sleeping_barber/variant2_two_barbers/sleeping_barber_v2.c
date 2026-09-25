/*
 * Sleeping Barber Problem - Variant 2: TWO barbers, 3 waiting chairs.
 * Sync tools: 2 semaphores (customers, barber_ready) + 1 mutex (seats).
 * Same shared chairs, same semaphores - both barbers pull from one queue.
 * macOS only: spawns Terminal.app windows to visualize the simulation live.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
#include <fcntl.h>
#include <time.h>

#define NUM_CHAIRS 3
#define NUM_BARBERS 2

#define CLR_RESET   "\033[0m"
#define CLR_RED     "\033[1;31m"
#define CLR_GREEN   "\033[1;32m"
#define CLR_YELLOW  "\033[1;33m"
#define CLR_BLUE    "\033[1;34m"
#define CLR_CYAN    "\033[1;36m"
#define CLR_MAGENTA "\033[1;35m"

/* Named semaphores: unnamed sem_init() is deprecated and unreliable on
 * macOS, so these are opened by name (sem_open) instead. */
sem_t *customers_sem;
sem_t *barber_ready_sem;
pthread_mutex_t seats_mutex;

int waiting_count = 0;
int shop_open = 1;
int total_served = 0;
int total_left = 0;
int num_customers = 10;

FILE *log_events, *log_barber, *log_room;

void timestamp(char *buf, size_t n) {
    time_t t = time(NULL);
    struct tm *lt = localtime(&t);
    strftime(buf, n, "%H:%M:%S", lt);
}

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

void draw_waiting_room(void) {
    char row[128] = "";
    for (int i = 0; i < NUM_CHAIRS; i++) {
        if (i < waiting_count)
            strcat(row, CLR_YELLOW "[C]" CLR_RESET);
        else
            strcat(row, "[ ]");
    }
    log_line(log_room, CLR_CYAN, "Chairs: %s  (%d/%d occupied)", row, waiting_count, NUM_CHAIRS);
}

void *barber_thread(void *arg) {
    int barber_id = *(int *)arg;
    free(arg);

    while (1) {
        log_line(log_barber, CLR_BLUE, "Barber %d is sleeping (no customers)...", barber_id);
        sem_wait(customers_sem);

        pthread_mutex_lock(&seats_mutex);
        if (waiting_count == 0 && !shop_open) {
            pthread_mutex_unlock(&seats_mutex);
            break;
        }
        waiting_count--;
        draw_waiting_room();
        pthread_mutex_unlock(&seats_mutex);

        log_line(log_barber, CLR_GREEN, "Barber %d woke up and is cutting hair", barber_id);
        sem_post(barber_ready_sem);
        sleep(1 + rand() % 3);
        log_line(log_barber, CLR_MAGENTA, "Barber %d finished a haircut", barber_id);
    }
    log_line(log_barber, CLR_RED, "Shop is closed, barber %d goes home", barber_id);
    return NULL;
}

void *customer_thread(void *arg) {
    int id = *(int *)arg;
    free(arg);

    pthread_mutex_lock(&seats_mutex);
    if (waiting_count < NUM_CHAIRS) {
        waiting_count++;
        log_line(log_events, CLR_GREEN, "Customer %d takes a seat", id);
        draw_waiting_room();
        pthread_mutex_unlock(&seats_mutex);

        sem_post(customers_sem);
        sem_wait(barber_ready_sem);

        log_line(log_events, CLR_YELLOW, "Customer %d is getting a haircut", id);
        pthread_mutex_lock(&seats_mutex);
        total_served++;
        pthread_mutex_unlock(&seats_mutex);
    } else {
        pthread_mutex_unlock(&seats_mutex);
        log_line(log_events, CLR_RED, "Customer %d leaves, no chairs free", id);
        pthread_mutex_lock(&seats_mutex);
        total_left++;
        pthread_mutex_unlock(&seats_mutex);
    }
    return NULL;
}

void *generator_thread(void *arg) {
    (void)arg;
    pthread_t customers[num_customers];

    for (int i = 0; i < num_customers; i++) {
        usleep(rand() % 1000000);
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
    for (int i = 0; i < NUM_BARBERS; i++)
        sem_post(customers_sem);
    return NULL;
}

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

void run_simulation(void) {
    system("mkdir -p logs");
    reset_logs();

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
        int *id = malloc(sizeof(int));
        *id = i + 1;
        pthread_create(&barbers[i], NULL, barber_thread, id);
    }
    pthread_create(&generator, NULL, generator_thread, NULL);

    pthread_join(generator, NULL);
    for (int i = 0; i < NUM_BARBERS; i++)
        pthread_join(barbers[i], NULL);

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

int main(void) {
    srand(time(NULL));
    print_banner();

    int choice;
    do {
        printf("1) Start simulation\n");
        printf("2) About this variant\n");
        printf("3) Exit\n");
        printf("Choice: ");
        if (scanf("%d", &choice) != 1) { choice = 3; break; }

        if (choice == 1) {
            printf("How many customers? ");
            scanf("%d", &num_customers);
            if (num_customers < 1) num_customers = 10;
            run_simulation();
        } else if (choice == 2) {
            print_about();
        }
    } while (choice != 3);

    printf("Goodbye.\n");
    return 0;
}
