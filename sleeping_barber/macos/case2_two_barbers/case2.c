/*
 * Sleeping Barber - Case 2: two barbers, 2 chairs, ONE shared set of
 * tools (a comb and a pair of scissors). A barber needs both tools to
 * cut hair, so the two barbers contend for the same two resources.
 * Demonstrates: sequential service, a barber blocking on tools already
 * held by the other barber, two customers arriving together, and the
 * two fixes needed once tools are shared - fixed lock ordering (comb
 * before scissors, always) to make deadlock structurally impossible,
 * and a FIFO ticket queue around tool requests so neither barber can
 * be starved by bad scheduling luck.
 * macOS: opens Terminal.app windows for Barber Room / Toolbox / Waiting Room.
 */

#include <pthread.h>
#include <semaphore.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>

#define CHAIRS        2
#define BARBERS       2
#define MAX_CUSTOMERS 64

typedef struct {
    int occupied;
    int id;
    sem_t *turn;
} chair_t;

chair_t chairs[CHAIRS];
sem_t *waiting_manager;   /* customer -> barbers: "someone is waiting" */
pthread_mutex_t chair_lock;

pthread_mutex_t comb_lock, scissors_lock;      /* the two shared tools */
pthread_mutex_t queue_lock;                     /* guards the ticket queue below */
pthread_cond_t  queue_cond;
int next_ticket = 0, now_serving = 0;           /* FIFO ticket queue: prevents starvation */
int tool_holder = 0;                            /* 0 = free, else barber id holding both tools */

void show_chairs(void);
void show_tools(void);
void log_room(const char *fmt, ...);
void log_barber(const char *fmt, ...);
void log_tools(const char *fmt, ...);
void setup_display(void);
void teardown_display(void);

/* ---------------- core synchronization logic ---------------- */

/* Every tool request goes through a ticket first: whoever asks earliest
 * is guaranteed to be served earliest, no matter how the OS schedules
 * the two barber threads afterwards - this is what prevents starvation.
 * Only once it's a barber's turn does it acquire the real locks, always
 * in the same fixed order (comb, then scissors) - since both barbers
 * follow that same order, neither can ever hold one tool while waiting
 * on the other, which is what makes deadlock structurally impossible. */
static void acquire_tools(int barber_id) {
    pthread_mutex_lock(&queue_lock);
    int ticket = next_ticket++;
    log_tools("Barber %d requests the toolset mutexes (comb_lock + scissors_lock), draws ticket %d", barber_id, ticket);
    if (ticket != now_serving) {
        log_tools("Barber %d waits in the FIFO tool queue behind ticket %d (now serving %d)", barber_id, now_serving, now_serving);
        show_tools();
    }
    while (ticket != now_serving)
        pthread_cond_wait(&queue_cond, &queue_lock);
    pthread_mutex_unlock(&queue_lock);

    log_tools("Barber %d's ticket %d is now serving - locks comb_lock first (fixed order)", barber_id, ticket);
    pthread_mutex_lock(&comb_lock);
    log_tools("Barber %d holds comb_lock, now locks scissors_lock (fixed order avoids deadlock)", barber_id);
    pthread_mutex_lock(&scissors_lock);

    pthread_mutex_lock(&queue_lock);
    tool_holder = barber_id;
    log_tools("Barber %d holds both comb_lock and scissors_lock - picks up comb, then scissors - ticket %d served", barber_id, ticket);
    show_tools();
    pthread_mutex_unlock(&queue_lock);
}

static void release_tools(int barber_id) {
    log_tools("Barber %d puts down the scissors, unlocks scissors_lock", barber_id);
    pthread_mutex_unlock(&scissors_lock);
    log_tools("Barber %d puts down the comb, unlocks comb_lock", barber_id);
    pthread_mutex_unlock(&comb_lock);

    pthread_mutex_lock(&queue_lock);
    tool_holder = 0;
    now_serving++;
    log_tools("Barber %d returns the toolset - ticket %d retired, now serving ticket %d", barber_id, now_serving - 1, now_serving);
    show_tools();
    pthread_cond_broadcast(&queue_cond);
    pthread_mutex_unlock(&queue_lock);
}

typedef struct { int id; } customer_args_t;

static void spawn_customer(pthread_t *t, void *(*fn)(void *), int id) {
    customer_args_t *a = malloc(sizeof(customer_args_t));
    a->id = id;
    pthread_create(t, NULL, fn, a);
}

void *customer(void *arg);

void *barber(void *arg) {
    int id = *(int *)arg;
    free(arg);

    while (1) {
        log_barber("Barber %d is asleep, no customers waiting", id);
        sem_wait(waiting_manager);

        pthread_mutex_lock(&chair_lock);
        int slot = -1;
        for (int i = 0; i < CHAIRS; i++)
            if (chairs[i].occupied) { slot = i; break; }

        if (slot == -1) {   /* nobody waiting: this was the shutdown post */
            pthread_mutex_unlock(&chair_lock);
            break;
        }

        int cust_id = chairs[slot].id;
        sem_t *cust_turn = chairs[slot].turn;
        chairs[slot].occupied = 0;
        show_chairs();
        pthread_mutex_unlock(&chair_lock);

        log_barber("Barber %d calls Customer %d to the chair", id, cust_id);
        sem_post(cust_turn);

        acquire_tools(id);
        log_barber("Barber %d begins cutting Customer %d's hair", id, cust_id);
        sleep(2);
        log_barber("Barber %d finishes Customer %d's haircut", id, cust_id);
        release_tools(id);
    }
    log_barber("Shop closed, Barber %d goes home", id);
    return NULL;
}

void *customer(void *arg) {
    customer_args_t *a = (customer_args_t *)arg;
    int id = a->id;
    free(a);

    char sem_name[32];
    snprintf(sem_name, sizeof(sem_name), "/sb_case2_turn_%d", id);
    sem_unlink(sem_name);
    sem_t *my_turn = sem_open(sem_name, O_CREAT | O_EXCL, 0644, 0);

    pthread_mutex_lock(&chair_lock);
    int slot = -1;
    for (int i = 0; i < CHAIRS; i++)
        if (!chairs[i].occupied) { slot = i; break; }

    if (slot == -1) {
        pthread_mutex_unlock(&chair_lock);
        log_room("Customer %d finds no free chair and leaves", id);
        sem_close(my_turn);
        sem_unlink(sem_name);
        return NULL;
    }

    chairs[slot].occupied = 1;
    chairs[slot].id = id;
    chairs[slot].turn = my_turn;
    show_chairs();
    pthread_mutex_unlock(&chair_lock);

    log_room("Customer %d takes a chair", id);
    sem_post(waiting_manager);
    sem_wait(my_turn);
    log_room("Customer %d moves to the barber chair", id);

    sem_close(my_turn);
    sem_unlink(sem_name);
    return NULL;
}

/* Returns 1 with *out set on success, 0 after invalid non-numeric input
 * (already drained to the next line), or -1 on EOF. The EOF case matters
 * because the drain loop below reads until '\n', and at EOF getchar()
 * keeps returning EOF forever, never '\n' - without checking for it here
 * a closed stdin (e.g. piped input running out) would spin the caller
 * forever instead of closing the shop. */
static int read_int(int *out) {
    if (scanf("%d", out) == 1) return 1;
    int c;
    while ((c = getchar()) != '\n' && c != EOF) { }
    return (c == EOF) ? -1 : 0;
}

int main(void) {
    setup_display();

    pthread_mutex_init(&chair_lock, NULL);
    pthread_mutex_init(&comb_lock, NULL);
    pthread_mutex_init(&scissors_lock, NULL);
    pthread_mutex_init(&queue_lock, NULL);
    pthread_cond_init(&queue_cond, NULL);
    /* Unlink first: if a previous run was killed before it could
     * sem_unlink this name, O_EXCL below would fail on a stale
     * leftover semaphore (wrong count) instead of creating a fresh
     * one at 0, making a barber think a customer already posted. */
    sem_unlink("/sb_case2_waiting_manager");
    waiting_manager = sem_open("/sb_case2_waiting_manager", O_CREAT | O_EXCL, 0644, 0);

    pthread_t barbers[BARBERS];
    for (int i = 0; i < BARBERS; i++) {
        int *id = malloc(sizeof(int)); *id = i + 1;
        pthread_create(&barbers[i], NULL, barber, id);
    }

    pthread_t customer_threads[MAX_CUSTOMERS];
    int customer_count = 0;
    int next_id = 1;

    printf("\n=== Sleeping Barber - Case 2 (2 barbers, %d chairs, 1 toolset) ===\n", CHAIRS);

    /* Seed the waiting room before the shop "opens" - e.g. enter
     * CHAIRS-1 here to leave exactly one vacancy, then send 2 arrivals
     * in the first round below to make them race for it. */
    int initial;
    do {
        printf("Customers already in the waiting room at open (0-%d): ", CHAIRS);
        int r = read_int(&initial);
        if (r == -1) { initial = 0; break; }   /* stdin closed: open with an empty room */
        if (r == 0) { initial = -1; }
    } while (initial < 0 || initial > CHAIRS);

    for (int i = 0; i < initial; i++)
        spawn_customer(&customer_threads[customer_count++], customer, next_id++);

    /* Each round: how many customers arrive together this time. 1 is a
     * plain arrival; 2 or more spawns that many customer threads at the
     * same instant, so they genuinely race for whatever chairs are free
     * right then, and both barbers genuinely contend for the shared
     * toolset once they're both busy. 0 closes the shop. */
    int n;
    do {
        printf("\nCustomers arriving this round (0 to close the shop): ");
        int r = read_int(&n);
        if (r == -1) break;   /* stdin closed: close the shop */
        if (r == 0) continue;
        if (n < 0) { printf("Invalid input.\n"); continue; }
        if (n == 0) break;

        /* Cap to actual remaining capacity before announcing the range,
         * so the log matches who really gets spawned rather than what
         * was requested if MAX_CUSTOMERS is hit. */
        int round_count = n;
        if (customer_count + round_count > MAX_CUSTOMERS)
            round_count = MAX_CUSTOMERS - customer_count;

        if (round_count > 1)
            log_room("Customer %d through Customer %d arrive at the same time", next_id, next_id + round_count - 1);

        for (int i = 0; i < round_count; i++)
            spawn_customer(&customer_threads[customer_count++], customer, next_id++);
    } while (1);

    for (int i = 0; i < customer_count; i++)
        pthread_join(customer_threads[i], NULL);

    for (int i = 0; i < BARBERS; i++)
        sem_post(waiting_manager);   /* wake each barber one last time so it sees no one waiting and exits */
    for (int i = 0; i < BARBERS; i++)
        pthread_join(barbers[i], NULL);

    sem_close(waiting_manager);
    sem_unlink("/sb_case2_waiting_manager");
    pthread_mutex_destroy(&chair_lock);
    pthread_mutex_destroy(&comb_lock);
    pthread_mutex_destroy(&scissors_lock);
    pthread_mutex_destroy(&queue_lock);
    pthread_cond_destroy(&queue_cond);

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

FILE *f_room, *f_barber, *f_tools;

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
void log_tools(const char *fmt, ...) {
    va_list a; va_start(a, fmt); vlog(f_tools, CLR_CYAN, fmt, a); va_end(a);
}

void show_chairs(void) {
    fprintf(f_room, CLR_YELLOW "+---- BARBER CHAIRS ----+\n" CLR_RESET);
    fprintf(f_room, "| ");
    for (int i = 0; i < CHAIRS; i++)
        fprintf(f_room, chairs[i].occupied ? CLR_RED "[C%d] " CLR_RESET : CLR_GREEN "[  ] " CLR_RESET, chairs[i].id);
    fprintf(f_room, "|\n" CLR_YELLOW "+------------------------+\n\n" CLR_RESET);
    fflush(f_room);
}

void show_tools(void) {
    fprintf(f_tools, CLR_YELLOW "+---------- TOOLBOX ----------+\n" CLR_RESET);
    if (tool_holder)
        fprintf(f_tools, "| Held by: " CLR_RED "Barber %d" CLR_RESET "\n", tool_holder);
    else
        fprintf(f_tools, "| Held by: " CLR_GREEN "nobody" CLR_RESET "\n");
    fprintf(f_tools, "| Now serving ticket: %d (next: %d)\n", now_serving, next_ticket);
    fprintf(f_tools, CLR_YELLOW "+------------------------------+\n\n" CLR_RESET);
    fflush(f_tools);
}

static void get_screen_size(int *w, int *h) {
    *w = 1440; *h = 900;   /* fallback if the query below fails for any reason */
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

/* quadrant: 0=top-left, 1=top-right, 2=bottom-left, 3=bottom-right */
static void spawn_terminal(const char *title, const char *logfile, int quadrant) {
    static int screen_w = 0, screen_h = 0;
    if (screen_w == 0) get_screen_size(&screen_w, &screen_h);
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
    system(cmd);
}

void setup_display(void) {
    char cwd[512];
    getcwd(cwd, sizeof(cwd));
    system("mkdir -p logs");

    /* Absolute paths: a freshly opened terminal window starts in the
     * user's home directory, not this program's working directory, so
     * a relative "logs/..." path fails there with "No such file". */
    char room_path[600], barber_path[600], tools_path[600];
    snprintf(room_path, sizeof(room_path), "%s/logs/waiting_room.log", cwd);
    snprintf(barber_path, sizeof(barber_path), "%s/logs/barber_shop.log", cwd);
    snprintf(tools_path, sizeof(tools_path), "%s/logs/toolbox.log", cwd);

    f_room   = fopen(room_path, "w");
    f_barber = fopen(barber_path, "w");
    f_tools  = fopen(tools_path, "w");
    spawn_terminal("Barber Room", barber_path, 0);
    spawn_terminal("Toolbox", tools_path, 1);
    spawn_terminal("Waiting Room", room_path, 2);
    sleep(1);
}

void teardown_display(void) {
    fclose(f_room); fclose(f_barber); fclose(f_tools);
}
