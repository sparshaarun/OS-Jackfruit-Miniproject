/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Intentionally partial starter:
 *   - command-line shape is defined
 *   - key runtime data structures are defined
 *   - bounded-buffer skeleton is defined
 *   - supervisor / client split is outlined
 *
 * Students are expected to design:
 *   - the control-plane IPC implementation
 *   - container lifecycle and metadata synchronization
 *   - clone + namespace setup for each container
 *   - producer/consumer behavior for log buffering
 *   - signal handling and graceful shutdown
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 256
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define g_logbuf_CAPACITY 16
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;



typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    char log_path[PATH_MAX];
    struct container_record *next;
} container_record_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;


#define BUFFER_SIZE 32
#define LOG_MSG_SIZE 512

typedef struct {
    char container_id[32];
    char data[LOG_MSG_SIZE];
} log_msg_t;

typedef struct {
    log_msg_t buf[BUFFER_SIZE];
    int head, tail, count;
    int shutdown;

    pthread_mutex_t mtx;
    pthread_cond_t not_full;
    pthread_cond_t not_empty;
} bounded_buffer_t;

pthread_t g_consumer_tid;
bounded_buffer_t g_logbuf;


typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct {
    int status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int log_write_fd;
    int pipe_fd;
} child_config_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    int should_stop;
    pthread_t logger_thread;
    bounded_buffer_t g_logbuf;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;


#define MAX_CONTAINERS 20

typedef struct container {
    char id[32];
    pid_t pid;
    int running;
    struct container *next;
} container_t;

container_t *container_list = NULL;

volatile int running_flag = 1;

void handle_sig(int sig) {
    (void)sig;
    running_flag = 0;
}

void buffer_init(bounded_buffer_t *b) {
    b->head = b->tail = b->count = 0;
    b->shutdown = 0;
    pthread_mutex_init(&b->mtx, NULL);
    pthread_cond_init(&b->not_full, NULL);
    pthread_cond_init(&b->not_empty, NULL);
}

void buffer_push(bounded_buffer_t *b, log_msg_t *msg) {
    pthread_mutex_lock(&b->mtx);

    while (b->count == BUFFER_SIZE && !b->shutdown)
        pthread_cond_wait(&b->not_full, &b->mtx);

    if (b->shutdown) {
        pthread_mutex_unlock(&b->mtx);
        return;
    }

    b->buf[b->tail] = *msg;
    b->tail = (b->tail + 1) % BUFFER_SIZE;
    b->count++;

    pthread_cond_signal(&b->not_empty);
    pthread_mutex_unlock(&b->mtx);
}

int buffer_pop(bounded_buffer_t *b, log_msg_t *msg) {
    pthread_mutex_lock(&b->mtx);

    while (b->count == 0 && !b->shutdown)
        pthread_cond_wait(&b->not_empty, &b->mtx);

    if (b->count == 0 && b->shutdown) {
        pthread_mutex_unlock(&b->mtx);
        return 0;
    }

    *msg = b->buf[b->head];
    b->head = (b->head + 1) % BUFFER_SIZE;
    b->count--;

    pthread_cond_signal(&b->not_full);
    pthread_mutex_unlock(&b->mtx);
    return 1;
}

void buffer_begin_shutdown(bounded_buffer_t *b) {
    pthread_mutex_lock(&b->mtx);
    b->shutdown = 1;
    pthread_cond_broadcast(&b->not_full);
    pthread_cond_broadcast(&b->not_empty);
    pthread_mutex_unlock(&b->mtx);
}

typedef struct {
    int fd;
    char id[32];
} producer_arg_t;

void *producer_thread(void *arg) {
    producer_arg_t *p = (producer_arg_t *)arg;

    char buf[LOG_MSG_SIZE];
    ssize_t n;

    while ((n = read(p->fd, buf, sizeof(buf)-1)) > 0) {
        buf[n] = '\0';

        log_msg_t msg;
        strncpy(msg.container_id, p->id, sizeof(msg.container_id)-1);
        strncpy(msg.data, buf, sizeof(msg.data)-1);

        buffer_push(&g_logbuf, &msg);
    }

    close(p->fd);
    free(p);
    return NULL;
}

void *consumer_thread(void *arg) {
    (void)arg;

    while (1) {
        log_msg_t msg;

        if (!buffer_pop(&g_logbuf, &msg))
            break;

        char filename[64];
        snprintf(filename, sizeof(filename), "%s.log", msg.container_id);

        FILE *f = fopen(filename, "a");
        if (f) {
            fprintf(f, "%s", msg.data);
            fclose(f);
        }
    }

    return NULL;
}


int run_container_direct(char *rootfs, char **cmd);

#define STACK_SIZE (1024 * 1024)
static char child_stack[STACK_SIZE];

int container_main(void *arg) {
    char **args = (char **)arg;

    sethostname("container", 9);

    if (chroot(args[0]) != 0) {
        perror("chroot failed");
        exit(1);
    }

    chdir("/");

    mount("proc", "/proc", "proc", 0, NULL);

    execvp(args[1], &args[1]);

    perror("exec failed");
    return 1;
}

int run_container_direct(char *rootfs, char **cmd) {
    char *args[] = { rootfs, cmd[0], NULL };

    int flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD;

    pid_t pid = clone(container_main, child_stack + STACK_SIZE, flags, args);

    if (pid < 0) {
        perror("clone failed");
        return -1;
    }

    waitpid(pid, NULL, 0);
    return 0;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag,
                          const char *value,
                          unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;

    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }

    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }

    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req,
                                int argc,
                                char *argv[],
                                int start_index)
{
    int i;

    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nice_value;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }

        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr,
                        "Invalid value for --nice (expected -20..19): %s\n",
                        argv[i + 1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }

        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }

    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }

    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING:
        return "starting";
    case CONTAINER_RUNNING:
        return "running";
    case CONTAINER_STOPPED:
        return "stopped";
    case CONTAINER_KILLED:
        return "killed";
    case CONTAINER_EXITED:
        return "exited";
    default:
        return "unknown";
    }
}



/*
 * TODO:
 * Implement producer-side insertion into the bounded buffer.
 *
 * Requirements:
 *   - block or fail according to your chosen policy when the buffer is full
 *   - wake consumers correctly
 *   - stop cleanly if shutdown begins
 */
int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    (void)buffer;
    (void)item;
    return -1;
}

/*
 * TODO:
 * Implement consumer-side removal from the bounded buffer.
 *
 * Requirements:
 *   - wait correctly while the buffer is empty
 *   - return a useful status when shutdown is in progress
 *   - avoid races with producers and shutdown
 */
int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    (void)buffer;
    (void)item;
    return -1;
}

/*
 * TODO:
 * Implement the logging consumer thread.
 *
 * Suggested responsibilities:
 *   - remove log chunks from the bounded buffer
 *   - route each chunk to the correct per-container log file
 *   - exit cleanly when shutdown begins and pending work is drained
 */
void *logging_thread(void *arg)
{
    (void)arg;
    return NULL;
}

/*
 * TODO:
 * Implement the clone child entrypoint.
 *
 * Required outcomes:
 *   - isolated PID / UTS / mount context
 *   - chroot or pivot_root into rootfs
 *   - working /proc inside container
 *   - stdout / stderr redirected to the supervisor logging path
 *   - configured command executed inside the container
 */
int child_fn(void *arg)
{
    child_config_t *config = (child_config_t *)arg;

    sethostname(config->id, strlen(config->id));

    if (chroot(config->rootfs) != 0) {
        perror("chroot");
        return 1;
    }

    chdir("/");

    mount("proc", "/proc", "proc", 0, NULL);

    // 🔥 THIS PART (must exist)
    dup2(config->pipe_fd, STDOUT_FILENO);
    dup2(config->pipe_fd, STDERR_FILENO);
    close(config->pipe_fd);

    execlp("sh", "sh", "-c", config->command, NULL);

    perror("exec");
    return 1;
}
int register_with_monitor(int monitor_fd,
                          const char *container_id,
                          pid_t host_pid,
                          unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;

    return 0;
}

int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;

    return 0;
}

/*
 * TODO:
 * Implement the long-running supervisor process.
 *
 * Suggested responsibilities:
 *   - create and bind the control-plane IPC endpoint
 *   - initialize shared metadata and the bounded buffer
 *   - start the logging thread
 *   - accept control requests and update container state
 *   - reap children and respond to signals
 */

static void reap_children(int sig)
{
    (void)sig;

    pid_t p;

    while ((p = waitpid(-1, NULL, WNOHANG)) > 0) {
        container_t *c = container_list;

        while (c) {
            if (c->pid == p) {
                c->running = 0;
                break;
            }
            c = c->next;
        }
    }
}

static int run_supervisor(const char *rootfs)
{
    signal(SIGINT, handle_sig);
    signal(SIGTERM, handle_sig);

    int server_fd;
    struct sockaddr_un addr;

    server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) return 1;

    unlink(CONTROL_PATH);

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) return 1;
    if (listen(server_fd, 5) < 0) return 1;

    buffer_init(&g_logbuf);
    pthread_create(&g_consumer_tid, NULL, consumer_thread, NULL);

    printf("Supervisor listening on %s\n", CONTROL_PATH);
    signal(SIGCHLD, reap_children);
    while (running_flag) {

        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) continue;

        control_request_t req;
        memset(&req, 0, sizeof(req));

        ssize_t n = read(client_fd, &req, sizeof(req));
        if (n != sizeof(req)) {
            close(client_fd);
            continue;
        }

        if (req.kind == CMD_START || req.kind == CMD_RUN) {

            int pipefd[2];
            if (pipe(pipefd) < 0) {
                close(client_fd);
                continue;
            }

            char *stack = malloc(STACK_SIZE);
            if (!stack) {
                close(client_fd);
                continue;
            }

            child_config_t *config = malloc(sizeof(child_config_t));
            if (!config) {
                close(client_fd);
                continue;
            }

            memset(config, 0, sizeof(*config));

            strncpy(config->id, req.container_id, sizeof(config->id)-1);
            strncpy(config->rootfs, req.rootfs, sizeof(config->rootfs)-1);
            strncpy(config->command, req.command, sizeof(config->command)-1);

            config->pipe_fd = pipefd[1];

            int flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD;

            pid_t pid = clone(child_fn, stack + STACK_SIZE, flags, config);

	    int fd = open("/dev/container_monitor", O_RDWR);
if (fd < 0) {
    perror("open monitor");
} else {
    struct monitor_request mreq;

    mreq.pid = pid;
    strncpy(mreq.container_id, req.container_id, sizeof(mreq.container_id) - 1);
    mreq.container_id[31] = '\0';

    mreq.soft_limit_bytes = 50UL * 1024 * 1024;
    mreq.hard_limit_bytes = 80UL * 1024 * 1024;

printf("DEBUG → pid=%d soft=%lu hard=%lu\n",
       mreq.pid,
       mreq.soft_limit_bytes,
       mreq.hard_limit_bytes);

    if (ioctl(fd, MONITOR_REGISTER, &mreq) < 0) {
        perror("ioctl register");
    }

    close(fd);
}

            if (pid < 0) {
                close(client_fd);
                continue;
            }

            container_t *c = malloc(sizeof(container_t));
            strcpy(c->id, req.container_id);
            c->pid = pid;
            c->running = 1;
            c->next = container_list;
            container_list = c;

            close(pipefd[1]);

            producer_arg_t *p = malloc(sizeof(*p));
            p->fd = pipefd[0];
            strncpy(p->id, req.container_id, sizeof(p->id)-1);

            pthread_t tid;
            pthread_create(&tid, NULL, producer_thread, p);
            pthread_detach(tid);

            if (req.kind == CMD_RUN) {
                waitpid(pid, NULL, 0);
                c->running = 0;
            }
        }

        else if (req.kind == CMD_PS) {
            container_t *c = container_list;
            printf("\nID\tPID\tSTATE\n");
            while (c) {
                printf("%s\t%d\t%s\n",
                       c->id,
                       c->pid,
                       c->running ? "running" : "exited");
                c = c->next;
            }
            printf("\n");
        }

        else if (req.kind == CMD_STOP) {
            container_t *c = container_list;
            while (c) {
                if (strcmp(c->id, req.container_id) == 0) {
                    kill(c->pid, SIGKILL);
                    c->running = 0;
                    printf("Stopped container %s\n", c->id);
                    break;
                }
                c = c->next;
            }
        }

        close(client_fd);

    }

    buffer_begin_shutdown(&g_logbuf);
    pthread_join(g_consumer_tid, NULL);

    close(server_fd);
    unlink(CONTROL_PATH);

    return 0;
}



/*
 * TODO:
 * Implement the client-side control request path.
 *
 * The CLI commands should use a second IPC mechanism distinct from the
 * logging pipe. A UNIX domain socket is the most direct option, but a
 * FIFO or shared memory design is also acceptable if justified.
 */
static int send_control_request(const control_request_t *req)
{
    int sock;
    struct sockaddr_un addr;

    sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect (is supervisor running?)");
        close(sock);
        return 1;
    }

    ssize_t n = write(sock, req, sizeof(*req));
    if (n != sizeof(*req)) {
        perror("write failed");
    }

    close(sock);
    return 0;
}


static int cmd_start(const char *id, const char *rootfs, const char *command)
{
    control_request_t req;
    memset(&req, 0, sizeof(req));

    req.kind = CMD_START;
    strncpy(req.container_id, id, sizeof(req.container_id) - 1);
    strncpy(req.rootfs, rootfs, sizeof(req.rootfs) - 1);
    strncpy(req.command, command, sizeof(req.command) - 1);

    return send_control_request(&req);
}

static int cmd_run(const char *id, const char *rootfs, const char *command)
{
    control_request_t req;
    memset(&req, 0, sizeof(req));

    req.kind = CMD_RUN;
    strncpy(req.container_id, id, sizeof(req.container_id) - 1);
    strncpy(req.rootfs, rootfs, sizeof(req.rootfs) - 1);
    strncpy(req.command, command, sizeof(req.command) - 1);

    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;

    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;

    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

static int cmd_stop(const char *id)
{
    control_request_t req;
    memset(&req, 0, sizeof(req));

    req.kind = CMD_STOP;
    strncpy(req.container_id, id, sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    // =========================
    // SUPERVISOR
    // =========================
    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }

    // =========================
    // START (background)
    // =========================
    else if (strcmp(argv[1], "start") == 0) {
        if (argc < 5) {
            fprintf(stderr, "Usage: %s start <id> <rootfs> <command>\n", argv[0]);
            return 1;
        }
        return cmd_start(argv[2], argv[3], argv[4]);
    }

    // =========================
    // RUN (foreground)
    // =========================
    else if (strcmp(argv[1], "run") == 0) {
        if (argc < 5) {
            fprintf(stderr, "Usage: %s run <id> <rootfs> <command>\n", argv[0]);
            return 1;
        }
        return cmd_run(argv[2], argv[3], argv[4]);
    }

    // =========================
    // PS
    // =========================
    else if (strcmp(argv[1], "ps") == 0) {
        return cmd_ps();
    }

    // =========================
    // STOP
    // =========================
    else if (strcmp(argv[1], "stop") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
            return 1;
        }
        return cmd_stop(argv[2]);
    }

    // =========================
    // LOGS (optional for later)
    // =========================
    else if (strcmp(argv[1], "logs") == 0) {
        return cmd_logs(argc, argv);  // keep if you have it
    }

    // =========================
    // UNKNOWN COMMAND
    // =========================
    else {
        usage(argv[0]);
        return 1;
    }
}
