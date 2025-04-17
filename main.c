#include <stdlib.h>
#include <sys/stat.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdbool.h>
#include <signal.h>
#include <fcntl.h>
#include <stdatomic.h>

#define FIFO_NAME_DEFAULT "/tmp/echo_server.fifo"
#define LOG_FILENAME_DEFAULT "/tmp/echo_server.log"
#define ALARM_TIME 15
#define BUFFER_SIZE 1024

char* fifo_name = FIFO_NAME_DEFAULT;
char* log_filename = LOG_FILENAME_DEFAULT;
int alarm_time = ALARM_TIME;

bool is_daemon = false;

volatile sig_atomic_t exit_mode = 0;
volatile sig_atomic_t alarm_flag = 0;
volatile sig_atomic_t sigusr_flag = 0;
volatile sig_atomic_t sighup_flag = 0;

atomic_ullong msg_count = 0;
atomic_ullong bytes_count = 0;
atomic_ullong alarm_count = 0;

void daemonize() {
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork error");
        exit(EXIT_FAILURE);
    }
    if (pid > 0) {
        exit(EXIT_SUCCESS);
    }

    if (setsid() < 0) {
        perror("setsid error");
        exit(EXIT_FAILURE);
    }

    if (freopen("/dev/null", "r", stdin) == NULL) {
        perror("freopen error");
        exit(EXIT_FAILURE);
    }
    if (freopen(log_filename, "w", stdout) == NULL) {
        perror("freopen error");
        exit(EXIT_FAILURE);
    }
    if (freopen(log_filename, "w", stderr) == NULL) {
        perror("freopen error");
        exit(EXIT_FAILURE);
    }

    is_daemon = true;
}

void print_stats() {
    printf("msg: %llu, bytes: %llu, alarm count: %llu\n", msg_count, bytes_count, alarm_count);
    fflush(stdout);
}

void handle_signal(int sig) {
    switch (sig) {
        case SIGINT:
            exit_mode = 1;
            break;
        case SIGTERM:
            exit_mode = 2;
            break;
        case SIGALRM:
            alarm_flag = 1;
            break;
        case SIGUSR1:
            sigusr_flag = 1;
            break;
        case SIGHUP:
            sighup_flag = 1;
            break;
        default:
            break;
    }
}

void external_handler() {
    if (alarm_flag) {
        printf("diag msg: waiting for data\n");
        fflush(stdout);
        alarm_count++;
        alarm_flag = 0;
        alarm(alarm_time);
    } if (sighup_flag) {
        if (!is_daemon) {
            daemonize();
            printf("daemonized via SIGHUP\n");
            fflush(stdout);
            print_stats();
            alarm(alarm_time);
        }
        sighup_flag = 0;
    }
    if (sigusr_flag) {
        print_stats();
        sigusr_flag = 0;
    }
}

int main(int argc, char *argv[]) {
    int opt;
    bool daemon_mode = false;

    while ((opt = getopt(argc, argv, "df:l:t:")) != -1) {
        switch (opt) {
            case 'd':
                daemon_mode = true;
                break;
            case 'f':
                fifo_name = optarg;
                break;
            case 'l':
                log_filename = optarg;
                break;
            case 't':
                alarm_time = atoi(optarg); 
                break;
            default: 
                fprintf(stderr, "usage: %s [-d] [-f fifo_name] [-log_filename]\n", argv[0]);
                exit(EXIT_FAILURE);
        }
    }

    if (mkfifo(fifo_name, 0600) == -1) {
        if (errno == EEXIST) {
            struct stat st;
            if (stat(fifo_name, &st) == -1) {
                perror("stat error");
                exit(EXIT_FAILURE);
            }
            if (!S_ISFIFO(st.st_mode)) {
                fprintf(stderr, "file %s exists and is not a fifo\n", fifo_name);
                exit(EXIT_FAILURE);
            }
        }
        else {
            perror("mkfifo error");
            exit(EXIT_FAILURE);
        }
    }

    struct sigaction sa = {.sa_handler = handle_signal};
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGTERM, &sa, NULL) == -1) { perror("sigaction SIGTERM"); exit(EXIT_FAILURE); }
    if (sigaction(SIGINT,  &sa, NULL) == -1) { perror("sigaction SIGINT");  exit(EXIT_FAILURE); }
    if (sigaction(SIGALRM, &sa, NULL) == -1) { perror("sigaction SIGALRM"); exit(EXIT_FAILURE); }
    if (sigaction(SIGUSR1, &sa, NULL) == -1) { perror("sigaction SIGUSR1"); exit(EXIT_FAILURE); }
    if (sigaction(SIGHUP,  &sa, NULL) == -1) { perror("sigaction SIGHUP");  exit(EXIT_FAILURE); }
    signal(SIGQUIT, SIG_IGN);

    if (daemon_mode) {
        daemonize();
    }

    alarm(alarm_time);

    while (!exit_mode) {
        external_handler();
        int fd = open(fifo_name, O_RDONLY);
        if (fd == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("open error");
            exit(EXIT_FAILURE);
        }
        char buf[BUFFER_SIZE];
        size_t total_read = 0;
        ssize_t n;
        char last_char = '\0';

        while ((n = read(fd, buf, BUFFER_SIZE - 1)) != 0) {
            if (n == -1) {
                if (errno != EINTR) {
                    perror("read error");
                    close(fd);
                    exit(EXIT_FAILURE);
                }
                if (exit_mode == 2) {
                    if (close(fd) < 0) {
                        perror("close error");
                        exit(EXIT_FAILURE);
                    }
                    break;
                }
                external_handler();
                continue;
            }
            buf[n] = '\0';
            printf("%s", buf);
            fflush(stdout);
            total_read += n;
            last_char = buf[n - 1];
        }

        if (exit_mode == 2) {
            break;
        }

        if (total_read > 0) {
            if (last_char != '\n') {
                printf("\n");
                fflush(stdout);
                total_read++;
            }
            bytes_count += total_read;
        }

        msg_count++;
        if (close(fd) < 0) {
            perror("close error");
            exit(EXIT_FAILURE);
        }
    }

    printf("exiting\n");
    print_stats();

    if (unlink(fifo_name) == -1) {
        perror("unlink error");
        exit(EXIT_FAILURE);
    }

    return 0;
}
