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
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16
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

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    int head;
    int tail;
    int count;
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
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int log_write_fd;
} child_config_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
} supervisor_ctx_t;

/* ---------------- BOUNDED BUFFER ---------------- */

int bounded_buffer_push(bounded_buffer_t *b, const log_item_t *item)
{
    pthread_mutex_lock(&b->mutex);

    while (b->count == LOG_BUFFER_CAPACITY && !b->shutting_down)
        pthread_cond_wait(&b->not_full, &b->mutex);

    if (b->shutting_down) {
        pthread_mutex_unlock(&b->mutex);
        return -1;
    }

    b->items[b->tail] = *item;
    b->tail = (b->tail + 1) % LOG_BUFFER_CAPACITY;
    b->count++;

    pthread_cond_signal(&b->not_empty);
    pthread_mutex_unlock(&b->mutex);
    return 0;
}

int bounded_buffer_pop(bounded_buffer_t *b, log_item_t *item)
{
    pthread_mutex_lock(&b->mutex);

    while (b->count == 0 && !b->shutting_down)
        pthread_cond_wait(&b->not_empty, &b->mutex);

    if (b->count == 0 && b->shutting_down) {
        pthread_mutex_unlock(&b->mutex);
        return -1;
    }

    *item = b->items[b->head];
    b->head = (b->head + 1) % LOG_BUFFER_CAPACITY;
    b->count--;

    pthread_cond_signal(&b->not_full);
    pthread_mutex_unlock(&b->mutex);
    return 0;
}

/* ---------------- LOGGING THREAD ---------------- */

void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = arg;
    log_item_t item;

    while (bounded_buffer_pop(&ctx->log_buffer, &item) == 0) {

        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.container_id);

        int fd = open(path, O_CREAT | O_WRONLY | O_APPEND, 0644);

        if (fd >= 0) {
            write(fd, item.data, item.length);
            close(fd);
        }
    }

    return NULL;
}

/* ---------------- CONTAINER CHILD ---------------- */

int child_fn(void *arg)
{
    child_config_t *cfg = arg;

    sethostname(cfg->id, strlen(cfg->id));

    if (chroot(cfg->rootfs) != 0) {
        perror("chroot");
        return 1;
    }

    chdir("/");

    mount("proc", "/proc", "proc", 0, NULL);

    if (cfg->nice_value != 0)
        nice(cfg->nice_value);

    dup2(cfg->log_write_fd, STDOUT_FILENO);
    dup2(cfg->log_write_fd, STDERR_FILENO);

    close(cfg->log_write_fd);

    char *const argv[] = { cfg->command, NULL };
    execvp(cfg->command, argv);

    perror("execvp");
    return 1;
}

/* ---------------- SUPERVISOR ---------------- */

static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    mkdir(LOG_DIR, 0755);

    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);

    pthread_mutex_init(&ctx.log_buffer.mutex, NULL);
    pthread_cond_init(&ctx.log_buffer.not_empty, NULL);
    pthread_cond_init(&ctx.log_buffer.not_full, NULL);

    pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);

    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));

    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, CONTROL_PATH);

    unlink(CONTROL_PATH);

    bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(ctx.server_fd, 5);

    printf("Supervisor running with rootfs: %s\n", rootfs);

    while (1) {

        int client = accept(ctx.server_fd, NULL, NULL);

        control_request_t req;
        read(client, &req, sizeof(req));

        if (req.kind == CMD_START || req.kind == CMD_RUN) {

            child_config_t cfg;
            memset(&cfg, 0, sizeof(cfg));

            strncpy(cfg.id, req.container_id, CONTAINER_ID_LEN);
            strncpy(cfg.rootfs, req.rootfs, PATH_MAX);
            strncpy(cfg.command, req.command, CHILD_COMMAND_LEN);
            cfg.nice_value = req.nice_value;

            int pipefd[2];
            pipe(pipefd);

            cfg.log_write_fd = pipefd[1];

            void *stack = malloc(STACK_SIZE);

            pid_t pid = clone(child_fn,
                              stack + STACK_SIZE,
                              CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                              &cfg);

            if (pid > 0) {

                struct monitor_request mreq;
                memset(&mreq, 0, sizeof(mreq));

                mreq.pid = pid;
                strncpy(mreq.container_id, req.container_id, sizeof(mreq.container_id)-1);
                mreq.soft_limit_bytes = req.soft_limit_bytes;
                mreq.hard_limit_bytes = req.hard_limit_bytes;

                ioctl(ctx.monitor_fd, MONITOR_REGISTER, &mreq);

                printf("Started container %s with pid %d\n",
                       req.container_id, pid);
            }

            close(pipefd[1]);

            log_item_t item;

            while (1) {
                ssize_t n = read(pipefd[0], item.data, LOG_CHUNK_SIZE);
                if (n <= 0)
                    break;

                strncpy(item.container_id, req.container_id, CONTAINER_ID_LEN);
                item.length = n;

                bounded_buffer_push(&ctx.log_buffer, &item);
            }

            close(pipefd[0]);
        }

        close(client);

        int status;
        pid_t pid = waitpid(-1, &status, WNOHANG);

        if (pid > 0)
            printf("Container process %d exited\n", pid);
    }

    return 0;
}

/* ---------------- CLIENT SIDE ---------------- */

static int send_control_request(const control_request_t *req)
{
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));

    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, CONTROL_PATH);

    connect(sock, (struct sockaddr *)&addr, sizeof(addr));

    write(sock, req, sizeof(*req));

    close(sock);
    return 0;
}

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;
    memset(&req, 0, sizeof(req));

    req.kind = CMD_START;

    strncpy(req.container_id, argv[2], sizeof(req.container_id)-1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs)-1);
    strncpy(req.command, argv[4], sizeof(req.command)-1);

    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    return send_control_request(&req);
}

int main(int argc, char *argv[])
{
    if (strcmp(argv[1], "supervisor") == 0)
        return run_supervisor(argv[2]);

    if (strcmp(argv[1], "start") == 0)
        return cmd_start(argc, argv);

    return 0;
}
