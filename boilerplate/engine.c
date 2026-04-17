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
#include <sys/prctl.h>
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
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)
#define MONITOR_DEV "/dev/container_monitor"

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

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

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
} child_config_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    int should_stop;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

/* Global pointer used by signal handlers to reach the supervisor context */
static supervisor_ctx_t *g_ctx = NULL;

/* ─────────────────────────────────────────────────────────────────────── */

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

/* ═══════════════════════════════════════════════════════════════════════
 *  BOUNDED BUFFER — init / destroy / shutdown  (skeletons from boilerplate)
 * ═══════════════════════════════════════════════════════════════════════ */

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;

    memset(buffer, 0, sizeof(*buffer));

    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0)
        return rc;

    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) {
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    rc = pthread_cond_init(&buffer->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buffer->not_empty);
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buffer)
{
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer)
{
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutting_down = 1;
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  BOUNDED BUFFER — producer (push) / consumer (pop)  [Task 3]
 *
 *  Why mutex + two condition variables?
 *  A single mutex protects the shared head/tail/count fields so no two
 *  threads corrupt them simultaneously.  Two separate condition variables
 *  (not_full, not_empty) let producers and consumers wait on exactly the
 *  condition they care about without spuriously waking the wrong side.
 *  Without synchronisation a producer could overwrite a slot the consumer
 *  has not yet read (lost data), or a consumer could read an uninitialised
 *  slot (corruption).
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * bounded_buffer_push — producer side.
 *
 * Blocks while the buffer is full (unless shutdown has started).
 * Inserts item at tail, advances tail, signals one waiting consumer.
 * Returns  0 on success, -1 if shutdown is in progress.
 */
int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);

    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;

    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/*
 * bounded_buffer_pop — consumer side.
 *
 * Blocks while the buffer is empty.
 * Returns -1 only when BOTH shutdown is set AND the buffer is empty,
 * so the consumer drains every remaining item before it exits.
 */
int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == 0 && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);

    if (buffer->count == 0 && buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;

    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  LOGGING CONSUMER THREAD  [Task 3]
 *
 *  Drains the bounded buffer and appends each chunk to the per-container
 *  log file.  The thread keeps running until bounded_buffer_pop returns -1
 *  (shutdown signalled AND buffer empty), guaranteeing no data is dropped.
 * ═══════════════════════════════════════════════════════════════════════ */

void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;

    while (bounded_buffer_pop(&ctx->log_buffer, &item) == 0) {
        char log_path[PATH_MAX];
        snprintf(log_path, sizeof(log_path),
                 "%s/%s.log", LOG_DIR, item.container_id);

        int fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd < 0) {
            fprintf(stderr, "[logger] cannot open %s: %s\n",
                    log_path, strerror(errno));
            continue;
        }

        size_t len = item.length > 0 ? item.length : strlen(item.data);
        ssize_t written = 0;
        while ((size_t)written < len) {
            ssize_t n = write(fd, item.data + written,
                              len - (size_t)written);
            if (n < 0) break;
            written += n;
        }
        /* Ensure each chunk ends with a newline in the log file */
        if (len > 0 && item.data[len - 1] != '\n')
            write(fd, "\n", 1);

        close(fd);
    }

    fprintf(stderr, "[logger] thread exiting — buffer drained\n");
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  PER-CONTAINER PIPE READER — producer thread  [Task 3]
 *
 *  Reads lines from the container's stdout/stderr pipe and pushes them
 *  into the bounded buffer.  One thread per container; detached so the
 *  supervisor does not need to join it explicitly.
 * ═══════════════════════════════════════════════════════════════════════ */

typedef struct {
    int               read_fd;
    char              container_id[CONTAINER_ID_LEN];
    bounded_buffer_t *log_buffer;
} pipe_reader_arg_t;

static void *pipe_reader_thread(void *arg)
{
    pipe_reader_arg_t *pra = (pipe_reader_arg_t *)arg;
    log_item_t item;
    char ch;
    size_t pos = 0;

    memset(&item, 0, sizeof(item));
    strncpy(item.container_id, pra->container_id, CONTAINER_ID_LEN - 1);

    /* Read one byte at a time and split on newlines into log_item_t chunks */
    while (read(pra->read_fd, &ch, 1) == 1) {
        item.data[pos++] = ch;
        if (ch == '\n' || pos == LOG_CHUNK_SIZE - 1) {
            item.data[pos] = '\0';
            item.length    = pos;
            bounded_buffer_push(pra->log_buffer, &item);
            pos = 0;
            memset(item.data, 0, sizeof(item.data));
        }
    }
    /* Flush any partial line left in the buffer */
    if (pos > 0) {
        item.data[pos] = '\0';
        item.length    = pos;
        bounded_buffer_push(pra->log_buffer, &item);
    }

    close(pra->read_fd);
    free(pra);
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  CONTAINER CHILD ENTRYPOINT  [Task 1]
 *
 *  Runs inside the new PID / UTS / mount namespaces created by clone().
 *  Sets up the isolated environment then exec()s the requested command.
 * ═══════════════════════════════════════════════════════════════════════ */

int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;

    /* Redirect stdout and stderr into the log pipe so the supervisor
     * can capture all container output through the logging pipeline. */
    if (dup2(cfg->log_write_fd, STDOUT_FILENO) < 0 ||
        dup2(cfg->log_write_fd, STDERR_FILENO) < 0) {
        perror("dup2");
        return 1;
    }
    close(cfg->log_write_fd);

    /* Set the hostname to the container ID inside the new UTS namespace */
    if (sethostname(cfg->id, strlen(cfg->id)) < 0)
        perror("sethostname");   /* non-fatal */

    /* Mount /proc so tools like ps work correctly inside the container */
    char proc_path[PATH_MAX];
    snprintf(proc_path, sizeof(proc_path), "%s/proc", cfg->rootfs);
    mkdir(proc_path, 0555);
    if (mount("proc", proc_path, "proc", 0, NULL) < 0)
        perror("mount proc");    /* non-fatal */

    /* chroot + chdir to make rootfs the container's filesystem root */
    if (chroot(cfg->rootfs) < 0) { perror("chroot"); return 1; }
    if (chdir("/")          < 0) { perror("chdir");  return 1; }

    /* Apply nice value for scheduling-experiment containers */
    if (cfg->nice_value != 0) {
        if (nice(cfg->nice_value) < 0)
            perror("nice");      /* non-fatal */
    }

    /* Ensure the child is killed automatically if the supervisor exits */
    prctl(PR_SET_PDEATHSIG, SIGKILL);

    /* Execute the configured command inside the container.
     * We use /bin/sh -c "<command>" so that multi-word commands like
     * "/bin/sleep 100" or shell built-ins work correctly.
     * cfg->command is the full command string from the CLI. */
    char *sh_argv[] = { "/bin/sh", "-c", cfg->command, NULL };
    char *envp[]    = {
        "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
        "TERM=xterm",
        NULL
    };
    execve("/bin/sh", sh_argv, envp);
    /* execve only returns on failure */
    perror("execve /bin/sh");
    return 1;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  KERNEL MONITOR INTEGRATION  [Task 4]
 * ═══════════════════════════════════════════════════════════════════════ */

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

/* ═══════════════════════════════════════════════════════════════════════
 *  SIGNAL HANDLERS  [Task 2]
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * SIGCHLD handler — reaps all exited children without blocking.
 *
 * The SA_NOCLDSTOP flag means we only get SIGCHLD on exit/kill, not on
 * stop/continue.  The loop with WNOHANG is required because multiple
 * children may exit between deliveries of a single SIGCHLD.
 */
static void sigchld_handler(int sig)
{
    (void)sig;
    int wstatus;
    pid_t pid;

    while ((pid = waitpid(-1, &wstatus, WNOHANG)) > 0) {
        if (!g_ctx) continue;

        pthread_mutex_lock(&g_ctx->metadata_lock);
        container_record_t *c = g_ctx->containers;
        while (c) {
            if (c->host_pid == pid) {
                if (WIFEXITED(wstatus)) {
                    c->state    = CONTAINER_EXITED;
                    c->exit_code = WEXITSTATUS(wstatus);
                } else if (WIFSIGNALED(wstatus)) {
                    c->state       = CONTAINER_KILLED;
                    c->exit_signal = WTERMSIG(wstatus);
                } else {
                    c->state = CONTAINER_STOPPED;
                }
                /* Clean up kernel monitor entry for this container */
                if (g_ctx->monitor_fd >= 0)
                    unregister_from_monitor(g_ctx->monitor_fd,
                                            c->id, c->host_pid);
                break;
            }
            c = c->next;
        }
        pthread_mutex_unlock(&g_ctx->metadata_lock);
    }
}

/* SIGINT / SIGTERM — tell the supervisor event loop to exit cleanly */
static void sigterm_handler(int sig)
{
    (void)sig;
    if (g_ctx) g_ctx->should_stop = 1;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  SUPERVISOR HELPERS — launch, ps, logs, stop  [Tasks 1, 2]
 * ═══════════════════════════════════════════════════════════════════════ */

/* 16 static clone stacks — one per concurrent container slot */
static char clone_stacks[16][STACK_SIZE];
static int  stack_slot = 0;

/*
 * Launch a new container, wire its log pipe, register with the kernel
 * monitor, add a metadata record, and spawn the producer thread.
 *
 * foreground == 1 → supervisor waits for the container to exit (run cmd).
 * foreground == 0 → supervisor returns immediately (start cmd).
 */
static int supervisor_launch(supervisor_ctx_t *ctx,
                             const control_request_t *req,
                             int foreground,
                             control_response_t *resp)
{
    /* Create the pipe that carries container stdout/stderr to the supervisor */
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        snprintf(resp->message, sizeof(resp->message),
                 "pipe() failed: %s", strerror(errno));
        resp->status = -1;
        return -1;
    }

    /* Build child configuration passed through clone() */
    child_config_t *cfg = malloc(sizeof(child_config_t));
    if (!cfg) {
        close(pipefd[0]); close(pipefd[1]);
        resp->status = -1;
        return -1;
    }
    memset(cfg, 0, sizeof(*cfg));
    strncpy(cfg->id,      req->container_id, CONTAINER_ID_LEN - 1);
    strncpy(cfg->rootfs,  req->rootfs,       PATH_MAX - 1);
    strncpy(cfg->command, req->command,      CHILD_COMMAND_LEN - 1);
    cfg->nice_value   = req->nice_value;
    cfg->log_write_fd = pipefd[1];

    /*
     * clone() with CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS creates:
     *   CLONE_NEWPID — container gets its own PID namespace (PID 1 inside)
     *   CLONE_NEWUTS — container gets its own hostname
     *   CLONE_NEWNS  — container gets its own mount namespace for chroot
     */
    int flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD;
    int slot  = stack_slot++ % 16;
    pid_t pid = clone(child_fn,
                      clone_stacks[slot] + STACK_SIZE,
                      flags, cfg);
    /* NOTE: we intentionally do NOT free(cfg) here.
     * cfg lives on the heap and the child process (created by clone) reads
     * cfg->rootfs, cfg->command, etc. before calling execve().  Freeing cfg
     * in the parent before the child has exec'd would be a use-after-free.
     * Once the child calls execve() the address space is replaced and the
     * memory is reclaimed automatically by the OS. */
    close(pipefd[1]);   /* supervisor only reads from the pipe */

    if (pid < 0) {
        close(pipefd[0]);
        snprintf(resp->message, sizeof(resp->message),
                 "clone() failed: %s", strerror(errno));
        resp->status = -1;
        return -1;
    }

    /* Allocate and populate the metadata record */
    container_record_t *rec = calloc(1, sizeof(container_record_t));
    strncpy(rec->id, req->container_id, CONTAINER_ID_LEN - 1);
    rec->host_pid         = pid;
    rec->started_at       = time(NULL);
    rec->state            = CONTAINER_RUNNING;
    rec->soft_limit_bytes = req->soft_limit_bytes;
    rec->hard_limit_bytes = req->hard_limit_bytes;
    snprintf(rec->log_path, sizeof(rec->log_path),
             "%s/%s.log", LOG_DIR, req->container_id);

    /* Prepend to the linked list (mutex-protected) */
    pthread_mutex_lock(&ctx->metadata_lock);
    rec->next       = ctx->containers;
    ctx->containers = rec;
    pthread_mutex_unlock(&ctx->metadata_lock);

    /* Register the container's host PID with the kernel memory monitor */
    if (ctx->monitor_fd >= 0)
        register_with_monitor(ctx->monitor_fd, req->container_id, pid,
                               req->soft_limit_bytes, req->hard_limit_bytes);

    /* Spawn the pipe-reader producer thread for this container */
    pipe_reader_arg_t *pra = malloc(sizeof(pipe_reader_arg_t));
    pra->read_fd    = pipefd[0];
    pra->log_buffer = &ctx->log_buffer;
    strncpy(pra->container_id, req->container_id, CONTAINER_ID_LEN - 1);
    pthread_t reader_tid;
    pthread_create(&reader_tid, NULL, pipe_reader_thread, pra);
    pthread_detach(reader_tid);   /* supervisor does not join this thread */

    if (foreground) {
        /* "run" command — block until the container finishes */
        int wstatus;
        waitpid(pid, &wstatus, 0);
        pthread_mutex_lock(&ctx->metadata_lock);
        rec->state = WIFEXITED(wstatus) ? CONTAINER_EXITED : CONTAINER_KILLED;
        if (WIFEXITED(wstatus))   rec->exit_code   = WEXITSTATUS(wstatus);
        if (WIFSIGNALED(wstatus)) rec->exit_signal  = WTERMSIG(wstatus);
        pthread_mutex_unlock(&ctx->metadata_lock);
        snprintf(resp->message, sizeof(resp->message),
                 "container '%s' (pid %d) finished", req->container_id, pid);
    } else {
        snprintf(resp->message, sizeof(resp->message),
                 "started '%s' pid=%d", req->container_id, pid);
    }
    resp->status = 0;
    return 0;
}

/* Build a formatted table of all tracked containers into resp->message */
static void supervisor_ps(supervisor_ctx_t *ctx, control_response_t *resp)
{
    int off = 0;
    off += snprintf(resp->message + off,
                    sizeof(resp->message) - (size_t)off,
                    "%-16s %-8s %-10s %-12s %-12s\n",
                    "ID", "PID", "STATE", "SOFT(MiB)", "HARD(MiB)");

    pthread_mutex_lock(&ctx->metadata_lock);
    container_record_t *c = ctx->containers;
    while (c && off < (int)sizeof(resp->message) - 2) {
        char line[128];
        int l = snprintf(line, sizeof(line),
                         "%-16s %-8d %-10s %-12lu %-12lu\n",
                         c->id, c->host_pid, state_to_string(c->state),
                         c->soft_limit_bytes >> 20,
                         c->hard_limit_bytes >> 20);
        if (off + l < (int)sizeof(resp->message)) {
            memcpy(resp->message + off, line, (size_t)l);
            off += l;
        }
        c = c->next;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);
    resp->message[off] = '\0';
    resp->status = 0;
}

/* Return the contents of a container's log file in resp->message */
static void supervisor_logs(supervisor_ctx_t *ctx,
                            const char *id,
                            control_response_t *resp)
{
    pthread_mutex_lock(&ctx->metadata_lock);
    container_record_t *c = ctx->containers;
    while (c && strcmp(c->id, id) != 0) c = c->next;
    char log_path[PATH_MAX];
    if (c) strncpy(log_path, c->log_path, PATH_MAX - 1);
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (!c) {
        snprintf(resp->message, sizeof(resp->message),
                 "no container '%s'", id);
        resp->status = -1;
        return;
    }

    int fd = open(log_path, O_RDONLY);
    if (fd < 0) {
        snprintf(resp->message, sizeof(resp->message),
                 "(log not yet created for '%s')", id);
        resp->status = 0;
        return;
    }
    ssize_t n = read(fd, resp->message, sizeof(resp->message) - 1);
    if (n < 0) n = 0;
    resp->message[n] = '\0';
    close(fd);
    resp->status = 0;
}

/*
 * Send SIGTERM to the container; if it does not exit within 3 seconds,
 * send SIGKILL.  This distinguishes graceful stop from forced kill in the
 * container metadata (updated by the SIGCHLD handler).
 */
static void supervisor_stop(supervisor_ctx_t *ctx,
                            const char *id,
                            control_response_t *resp)
{
    pthread_mutex_lock(&ctx->metadata_lock);
    container_record_t *c = ctx->containers;
    while (c && strcmp(c->id, id) != 0) c = c->next;
    pid_t pid             = c ? c->host_pid : -1;
    container_state_t st  = c ? c->state    : CONTAINER_EXITED;
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (pid < 0 || st != CONTAINER_RUNNING) {
        snprintf(resp->message, sizeof(resp->message),
                 "container '%s' is not running", id);
        resp->status = -1;
        return;
    }

    kill(pid, SIGTERM);

    /* Poll for up to 3 seconds for the container to exit */
    int i;
    for (i = 0; i < 30; i++) {
        usleep(100000);   /* 100 ms per iteration */
        pthread_mutex_lock(&ctx->metadata_lock);
        container_state_t cur = c->state;
        pthread_mutex_unlock(&ctx->metadata_lock);
        if (cur != CONTAINER_RUNNING) break;
    }
    if (i == 30) kill(pid, SIGKILL);   /* force kill after 3 s */

    snprintf(resp->message, sizeof(resp->message),
             "stop sent to '%s' (pid %d)", id, pid);
    resp->status = 0;
}

/* Route an incoming control request to the correct handler */
static void supervisor_dispatch(supervisor_ctx_t *ctx,
                                const control_request_t *req,
                                control_response_t *resp)
{
    memset(resp, 0, sizeof(*resp));
    switch (req->kind) {
    case CMD_START:
        supervisor_launch(ctx, req, 0, resp);
        break;
    case CMD_RUN:
        supervisor_launch(ctx, req, 1, resp);
        break;
    case CMD_PS:
        supervisor_ps(ctx, resp);
        break;
    case CMD_LOGS:
        supervisor_logs(ctx, req->container_id, resp);
        break;
    case CMD_STOP:
        supervisor_stop(ctx, req->container_id, resp);
        break;
    default:
        snprintf(resp->message, sizeof(resp->message),
                 "unknown command kind %d", req->kind);
        resp->status = -1;
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 *  SUPERVISOR MAIN  [Tasks 1, 2, 3, 4]
 * ═══════════════════════════════════════════════════════════════════════ */

static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    int rc;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd  = -1;
    ctx.monitor_fd = -1;
    g_ctx = &ctx;   /* expose to signal handlers */

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) {
        errno = rc;
        perror("pthread_mutex_init");
        return 1;
    }

    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) {
        errno = rc;
        perror("bounded_buffer_init");
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    /* 1) Ensure the per-run log directory exists */
    mkdir(LOG_DIR, 0755);

    /* 2) Open the kernel memory monitor device
     *    Not fatal if the module is not loaded — limits simply won't fire. */
    ctx.monitor_fd = open(MONITOR_DEV, O_RDWR);
    if (ctx.monitor_fd < 0)
        fprintf(stderr,
                "[supervisor] warning: cannot open %s (%s) — "
                "memory limits disabled\n",
                MONITOR_DEV, strerror(errno));
    else
        fprintf(stderr, "[supervisor] kernel monitor opened (%s)\n",
                MONITOR_DEV);

    /* 3) Create the UNIX domain socket that serves as the control channel.
     *    CLI clients connect here to send commands and receive responses.
     *    This is the second IPC mechanism, distinct from the log pipes. */
    unlink(CONTROL_PATH);
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    chmod(CONTROL_PATH, 0666);
    listen(ctx.server_fd, 8);

    /* 4) Install signal handlers
     *    SIGCHLD with SA_NOCLDSTOP so we only wake on exit, not stop/cont.
     *    SA_RESTART so system calls interrupted by SIGCHLD auto-restart. */
    struct sigaction sa_chld = {0};
    sa_chld.sa_handler = sigchld_handler;
    sa_chld.sa_flags   = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa_chld, NULL);

    struct sigaction sa_term = {0};
    sa_term.sa_handler = sigterm_handler;
    sa_term.sa_flags   = SA_RESTART;
    sigaction(SIGTERM, &sa_term, NULL);
    sigaction(SIGINT,  &sa_term, NULL);

    /* 5) Spawn the logging consumer thread */
    rc = pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);
    if (rc != 0) {
        errno = rc; perror("pthread_create logger");
        return 1;
    }

    fprintf(stderr, "[supervisor] ready.  rootfs=%s  socket=%s\n",
            rootfs, CONTROL_PATH);

    /* 6) Event loop — accept CLI connections, dispatch commands, respond */
    while (!ctx.should_stop) {
        struct timeval tv = {1, 0};   /* 1 s timeout to poll should_stop */
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(ctx.server_fd, &rfds);

        int sel = select(ctx.server_fd + 1, &rfds, NULL, NULL, &tv);
        if (sel < 0 && errno == EINTR) continue;
        if (sel <= 0) continue;

        int cli = accept(ctx.server_fd, NULL, NULL);
        if (cli < 0) continue;

        control_request_t  req;
        control_response_t resp;
        ssize_t n = recv(cli, &req, sizeof(req), 0);
        if (n == (ssize_t)sizeof(req)) {
            supervisor_dispatch(&ctx, &req, &resp);
        } else {
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message), "short read");
        }
        send(cli, &resp, sizeof(resp), 0);
        close(cli);
    }

    /* 7) Orderly shutdown */
    fprintf(stderr, "[supervisor] shutting down...\n");

    pthread_mutex_lock(&ctx.metadata_lock);
    container_record_t *c = ctx.containers;
    while (c) {
        if (c->state == CONTAINER_RUNNING) kill(c->host_pid, SIGTERM);
        c = c->next;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);
    sleep(2);

    /* Signal logger to drain and exit, then wait for it */
    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);
    fprintf(stderr, "[supervisor] logger thread joined\n");

    /* Free the metadata linked list */
    pthread_mutex_lock(&ctx.metadata_lock);
    c = ctx.containers;
    while (c) {
        container_record_t *nxt = c->next;
        free(c);
        c = nxt;
    }
    ctx.containers = NULL;
    pthread_mutex_unlock(&ctx.metadata_lock);

    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_mutex_destroy(&ctx.metadata_lock);
    if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);
    close(ctx.server_fd);
    unlink(CONTROL_PATH);

    fprintf(stderr, "[supervisor] done.\n");
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  CLI CLIENT — sends one request to the supervisor and prints response
 *  [Task 2 — second IPC mechanism: UNIX domain socket]
 * ═══════════════════════════════════════════════════════════════════════ */

static int send_control_request(const control_request_t *req)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr,
                "Cannot connect to supervisor at %s\n"
                "Is 'sudo ./engine supervisor <rootfs>' running?\n",
                CONTROL_PATH);
        close(fd);
        return 1;
    }

    /* Send the fixed-size request struct */
    if (send(fd, req, sizeof(*req), 0) != (ssize_t)sizeof(*req)) {
        perror("send"); close(fd); return 1;
    }

    /* Receive the fixed-size response struct and print the message */
    control_response_t resp;
    ssize_t n = recv(fd, &resp, sizeof(resp), 0);
    close(fd);

    if (n != (ssize_t)sizeof(resp)) {
        fprintf(stderr, "Incomplete response from supervisor\n");
        return 1;
    }

    printf("%s\n", resp.message);
    return resp.status == 0 ? 0 : 1;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  CLI COMMAND HANDLERS  (preserved verbatim from boilerplate)
 * ═══════════════════════════════════════════════════════════════════════ */

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs,       argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command,      argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs,       argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command,      argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;

    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;

    /*
     * TODO:
     * The supervisor should respond with container metadata.
     * Keep the rendering format simple enough for demos and debugging.
     */
    printf("Expected states include: %s, %s, %s, %s, %s\n",
           state_to_string(CONTAINER_STARTING),
           state_to_string(CONTAINER_RUNNING),
           state_to_string(CONTAINER_STOPPED),
           state_to_string(CONTAINER_KILLED),
           state_to_string(CONTAINER_EXITED));
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

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  MAIN  (preserved verbatim from boilerplate)
 * ═══════════════════════════════════════════════════════════════════════ */

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }

    if (strcmp(argv[1], "start") == 0)
        return cmd_start(argc, argv);

    if (strcmp(argv[1], "run") == 0)
        return cmd_run(argc, argv);

    if (strcmp(argv[1], "ps") == 0)
        return cmd_ps();

    if (strcmp(argv[1], "logs") == 0)
        return cmd_logs(argc, argv);

    if (strcmp(argv[1], "stop") == 0)
        return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
