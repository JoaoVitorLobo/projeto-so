#define _POSIX_C_SOURCE 200809L

#include "board.h"
#include "display.h"
#include "protocol.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/wait.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdatomic.h>
#include <errno.h>


#define CONTINUE_PLAY 0
#define NEXT_LEVEL 1
#define QUIT_GAME 2
#define LOAD_BACKUP 3
#define CREATE_BACKUP 4


volatile sig_atomic_t sigusr1_received = 0;

void handle_sigusr1() {
    sigusr1_received = 1;
}

typedef struct {
    char client_request_pipe[MAX_PIPE_PATH_LENGTH];
    char client_notification_pipe[MAX_PIPE_PATH_LENGTH];
} client_pipes_t;


typedef struct {
    client_pipes_t pipe_data[QUEUE_SIZE];
    int head;
    int tail;
} register_queue_t;

client_pipes_t dequeue(register_queue_t* register_queue, pthread_mutex_t* queue_mutex, sem_t* items, sem_t* empty);
void board_to_message(char *message, board_t* game_board, int victory, int game_over, int accumulated_points);

typedef struct {
    board_t *board;
    int ghost_index;
    int *thread_shutdown;
} ghost_thread_arg_t;

typedef struct {
    char *level_dir_name;
    int total_levels;
    int points;
    int client_id;
    register_queue_t *client_queue;
    pthread_mutex_t *queue_mutex;
    pthread_mutex_t *session_mutex;
    sem_t *items;
    sem_t *empty;
} session_thread_arg_t;

typedef struct {
    board_t *board;
    int* victory;
    int* game_over;
    int* accumulated_points;
    int client_notification_fd;
} ncurses_thread_arg_t;

typedef struct {
    session_thread_arg_t *thread_arg;
    board_t *board;
    int client_request_fd;
    int* result;
    int* points;
} pacman_thread_arg_t;

int create_backup() {
    // clear the terminal for process transition
    terminal_cleanup();

    pid_t child = fork();

    if(child != 0) {
        if (child < 0) {
            return -1;
        }

        return child;
    } else {
        return 0;
    }
}

void screen_refresh(board_t * game_board, int mode) {
    draw_board(game_board, mode);
    refresh_screen();     
}

void* ncurses_thread(void *arg) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    ncurses_thread_arg_t *ncurses_arg = (ncurses_thread_arg_t*) arg;
    board_t *board = ncurses_arg->board;
    int* victory = ncurses_arg->victory;
    int* game_over = ncurses_arg->game_over;
    int* accumulated_points = ncurses_arg->accumulated_points;
    int client_notification_fd = ncurses_arg->client_notification_fd;

    free(ncurses_arg);

    while (board->session_active) {
        sleep_ms(board->tempo);
        int data_size = sizeof(char) + (sizeof(int)*6) + (sizeof(char)* board->width * board->height);
        char message[data_size];

        pthread_rwlock_wrlock(&board->state_lock);
        board_to_message(message, board, *victory, *game_over, *accumulated_points);
        pthread_rwlock_unlock(&board->state_lock);

        write_full(client_notification_fd, message, data_size);
    }
    return NULL;
}

void* pacman_thread(void *arg) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    pacman_thread_arg_t *pacman_arg = (pacman_thread_arg_t*) arg;
    board_t *board = pacman_arg->board;
    int client_request_fd = pacman_arg->client_request_fd;
    int* points = pacman_arg->points;

    free(pacman_arg);

    pacman_t* pacman = &board->pacmans[0];

    int *retval = malloc(sizeof(int));

    while (true) {
        if(!pacman->alive) {
            *retval = LOAD_BACKUP;
            return (void*) retval;
        }

        char buffer[1];
        ssize_t n;
        n = read_full(client_request_fd, buffer, 1);
        if (n != 1) {
            return (void*) retval;
        }

        int op_code = buffer[0] - '0';

        if(op_code == OP_CODE_DISCONNECT){
            *retval = QUIT_GAME;
            return (void*) retval;
        }
        n = read_full(client_request_fd, buffer, 1);
        if (n != 1) {
            return (void*) retval;
        }

        char command = buffer[0];

        command_t* play;
        command_t c;

        c.command = command;
        c.turns = 1;
        play = &c;

        // KEEP PLAYING - just continue without moving
        if (play->command == 'K') {
            continue;
        }

        // QUIT
        if (play->command == 'Q') {
            *retval = QUIT_GAME;
            return (void*) retval;
        }
        // FORK
        if (play->command == 'G') {
            *retval = CREATE_BACKUP;
            return (void*) retval;
        }

        pthread_rwlock_rdlock(&board->state_lock);
        int result = move_pacman(board, 0, play);
        *points = pacman->points;
        pthread_rwlock_unlock(&board->state_lock);

        if (result == REACHED_PORTAL) {
            // Next level
            *retval = NEXT_LEVEL;
            return (void*)retval;
        }
        else if(result == DEAD_PACMAN) {
            // Restart from child, wait for child, then quit
            *retval = LOAD_BACKUP;
            return (void*) retval;
        }
        else{
            *retval = CONTINUE_PLAY;
        }
        
        sleep_ms(board->tempo * (1 + pacman->passo));
        
    }
    
    return (void*) retval;
}

void* ghost_thread(void *arg) {
    ghost_thread_arg_t *ghost_arg = (ghost_thread_arg_t*) arg;
    board_t *board = ghost_arg->board;
    int ghost_ind = ghost_arg->ghost_index;
    int *thread_shutdown = ghost_arg->thread_shutdown;

    free(ghost_arg);

    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    ghost_t* ghost = &board->ghosts[ghost_ind];

    while (true) {
        sleep_ms(board->tempo * (1 + ghost->passo));

        pthread_rwlock_rdlock(&board->state_lock);
        if (*thread_shutdown) {
            pthread_rwlock_unlock(&board->state_lock);
            pthread_exit(NULL);
        }
        
        move_ghost(board, ghost_ind, &ghost->moves[ghost->current_move%ghost->n_moves]);
        pthread_rwlock_unlock(&board->state_lock);
    }
}

void board_to_message(char *message, board_t* game_board, int victory, int game_over, int accumulated_points) {
    char *ptr = message;

    ptr[0] = (char)('0' + OP_CODE_BOARD); 
    ptr += 1;

    memcpy(ptr, &game_board->width, sizeof(int));
    ptr += sizeof(int);

    memcpy(ptr, &game_board->height, sizeof(int));
    ptr += sizeof(int);

    memcpy(ptr, &game_board->tempo, sizeof(int));
    ptr += sizeof(int);

    int vic = victory;
    memcpy(ptr, &vic, sizeof(int));
    ptr += sizeof(int);

    int eg = game_over;
    memcpy(ptr, &eg, sizeof(int));
    ptr += sizeof(int);

    memcpy(ptr, &accumulated_points, sizeof(int));
    ptr += sizeof(int);

    for (int i = 0; i < game_board->width * game_board->height; i++) {
        switch(game_board->board[i].content) {
            case 'W':
                ptr[i] = '#';
                break;
            case 'P':
                ptr[i] = 'C';
                break;
            case 'M':
                ptr[i] = 'M';
                break;
            default:
                if (game_board->board[i].has_dot) {
                    ptr[i] = '.';
                } else if (game_board->board[i].has_portal) {
                    ptr[i] = '@';
                } else {
                    ptr[i] = ' ';
                }
                break;
        }
    }
}

void create_ncurses_thread(pthread_t *ncurses_tid, board_t *game_board, int* victory, int* game_over, int* accumulated_points, int client_notification_fd){
    ncurses_thread_arg_t *ncurses_arg = malloc(sizeof(ncurses_thread_arg_t));
    if (ncurses_arg == NULL) {
        perror("malloc ncurses_arg");
        return;
    }
    ncurses_arg->board = game_board;
    ncurses_arg->victory = victory;
    ncurses_arg->game_over = game_over;
    ncurses_arg->accumulated_points = accumulated_points;
    ncurses_arg->client_notification_fd = client_notification_fd;
    pthread_create(ncurses_tid, NULL, ncurses_thread, ncurses_arg);
}

void create_pacman_thread(pthread_t *pacman_tid, board_t *game_board, int* accumulated_points, int client_request_fd){
    pacman_thread_arg_t* pacman_arg = malloc(sizeof(pacman_thread_arg_t));
    if (pacman_arg == NULL) {
        perror("malloc pacman_arg");
        return;
    }
    pacman_arg->board = game_board;
    pacman_arg->points = accumulated_points;
    pacman_arg->client_request_fd = client_request_fd;

    pthread_create(pacman_tid, NULL, pacman_thread, pacman_arg);
}

void create_all_ghosts_threads(pthread_t *ghost_tids, board_t *game_board, int* level_thread_shutdown){
    for (int i = 0; i < game_board->n_ghosts; i++) {
        ghost_thread_arg_t *ghost_arg = malloc(sizeof(ghost_thread_arg_t));
        if (ghost_arg == NULL) {
            perror("malloc ghost_arg");
            continue;
        }
        ghost_arg->board = game_board;
        ghost_arg->ghost_index = i;
        ghost_arg->thread_shutdown = level_thread_shutdown;
        pthread_create(&ghost_tids[i], NULL, ghost_thread, (void*) ghost_arg);
    }
}

void join_all_ghosts_threads(pthread_t *ghost_tids, int n_ghosts) {
    for (int i = 0; i < n_ghosts; i++) {
        pthread_join(ghost_tids[i], NULL);
    }
}

void* individual_session_thread(void *session_args) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &set, NULL);
    session_thread_arg_t *thread_arg = (session_thread_arg_t *) session_args;

    char *level_dir_name = thread_arg->level_dir_name;
    int total_levels = thread_arg->total_levels;
    register_queue_t* client_queue = thread_arg->client_queue;
    pthread_mutex_t* queue_mutex = thread_arg->queue_mutex;
    sem_t* items = thread_arg->items;
    sem_t* empty = thread_arg->empty;

    DIR* level_dir = opendir(level_dir_name);
    
    while(true) {
        client_pipes_t client_pipe_data = dequeue(client_queue, queue_mutex, items, empty);

        char *client_request_pipe = client_pipe_data.client_request_pipe;
        char *client_notification_pipe = client_pipe_data.client_notification_pipe;

        pthread_mutex_lock(thread_arg->session_mutex);
        sscanf(client_request_pipe, "/tmp/%d_request", &thread_arg->client_id);
        pthread_mutex_unlock(thread_arg->session_mutex);

        char message[2];
        message[0] = (char)('0' + OP_CODE_CONNECT);

        int client_request_fd = open(client_request_pipe, O_RDWR);
        int client_notification_fd = open(client_notification_pipe, O_RDWR);
        if (client_notification_fd < 0) {
            perror("open client fifo");
            message[1] = '1'; 
            return NULL;
        }
        message[1] = '0'; 
        write_full(client_notification_fd, message, sizeof(message));
        int accumulated_points = 0;
        int end_game = 0;
        int victory = 0;
        int game_over = 0;
        int current_level = 0;
        board_t game_board;

        pid_t parent_process = getpid(); // Only the parent process can create backups

        struct dirent* entry;
        while ((entry = readdir(level_dir)) != NULL && !end_game) {
            if (entry->d_name[0] == '.') continue;

            char *dot = strrchr(entry->d_name, '.');
            if (!dot) continue;

            if (strcmp(dot, ".lvl") == 0) {
                load_level(&game_board, entry->d_name, level_dir_name, accumulated_points);
                current_level++;

                pthread_t ncurses_tid, pacman_tid;
                pthread_t *ghost_tids = malloc(game_board.n_ghosts * sizeof(pthread_t));

                int level_thread_shutdown = 0;
                game_board.session_active = true; 

                create_pacman_thread(&pacman_tid, &game_board, &accumulated_points, client_request_fd);

                create_all_ghosts_threads(ghost_tids, &game_board, &level_thread_shutdown);

                create_ncurses_thread(&ncurses_tid, &game_board, &victory, &game_over, &accumulated_points, client_notification_fd);

                while(true) {
                    int *retval_pacman;

                    pthread_join(pacman_tid, (void**)&retval_pacman); 

                    pthread_rwlock_wrlock(&game_board.state_lock);
                    level_thread_shutdown = 1;
                    game_board.session_active = false;
                    pthread_rwlock_unlock(&game_board.state_lock);

                    join_all_ghosts_threads(ghost_tids, game_board.n_ghosts);
                    
                    pthread_join(ncurses_tid, NULL);

                    int result = *retval_pacman;
                    free(retval_pacman);

                    if(result == NEXT_LEVEL) {
                        screen_refresh(&game_board, DRAW_WIN);
                        sleep_ms(game_board.tempo);
                        if (current_level >= total_levels) {
                            end_game = 1;
                            victory = 1;
                        }
                        break;
                    }

                    if(result == CREATE_BACKUP) {
                        if (parent_process == getpid()) {
                            pid_t child = create_backup();
                            if (child == -1) {
                                // failed to fork
                                end_game = true;
                                break;
                            }
                            if (child > 0) {
                                int status;
                                wait(&status);

                                if (WIFEXITED(status)) {
                                    int code = WEXITSTATUS(status);
                                    
                                    if (code == 1) {
                                        terminal_init();
                                    }
                                    else { // End game or error
                                        end_game = true;
                                        break;
                                    }
                                }
                            } else {
                                terminal_init();
                            }

                        }
                    }

                    if(result == LOAD_BACKUP) {
                        if(getpid() != parent_process) {
                            terminal_cleanup();
                            unload_level(&game_board);
                            
                            close_debug_file();

                            if (closedir(level_dir) == -1) {
                                fprintf(stderr, "Failed to close directory\n");
                                continue;
                            }
                            continue;
                        } else {
                            // No backup process, game over
                            result = QUIT_GAME;
                        }
                    }

                    if(result == QUIT_GAME) {
                        screen_refresh(&game_board, DRAW_GAME_OVER); 
                        sleep_ms(game_board.tempo);
                        game_over = 1;
                        end_game = true;
                        game_board.session_active = false;
                        break;
                    }

                    free(ghost_tids);
                    
                    accumulated_points = game_board.pacmans[0].points;

                }
                int data_size = sizeof(char) + (sizeof(int)*6) + (sizeof(char)* game_board.width * game_board.height);
                char message[data_size];

                board_to_message(message, &game_board, victory, game_over, accumulated_points);

                write_full(client_notification_fd, message, data_size);
                
                unload_level(&game_board);
            }
        }
        close(client_request_fd);   
        close(client_notification_fd);
        
        rewinddir(level_dir);
    }
    if (level_dir != NULL) {
        closedir(level_dir);
    }
    return NULL;
}

int count_levels(DIR* level_dir) {
    int count = 0;
    struct dirent* entry;
    while ((entry = readdir(level_dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        char *dot = strrchr(entry->d_name, '.');
        if (!dot) continue;
        if (strcmp(dot, ".lvl") == 0) {
            count++;
        }
    }
    rewinddir(level_dir);
    return count;
}

void queue_init(register_queue_t* register_queue, pthread_mutex_t* queue_mutex, sem_t* items, sem_t* empty) {
    register_queue->head = register_queue->tail = 0;
    pthread_mutex_init(queue_mutex, NULL);
    sem_init(items, 0, 0);  
    sem_init(empty, 0, QUEUE_SIZE);
}

void enqueue(register_queue_t* register_queue, pthread_mutex_t* queue_mutex, sem_t* items, sem_t* empty, char *req, char *notif) {
    sem_wait(empty);    
    pthread_mutex_lock(queue_mutex);

    client_pipes_t pipe_data;
    strcpy(pipe_data.client_request_pipe, req);
    strcpy(pipe_data.client_notification_pipe, notif);

    register_queue->pipe_data[register_queue->tail] = pipe_data;
    register_queue->tail = (register_queue->tail + 1) % QUEUE_SIZE;

    pthread_mutex_unlock(queue_mutex);
    sem_post(items);  // sinaliza novo item
}

client_pipes_t dequeue(register_queue_t* register_queue, pthread_mutex_t* queue_mutex, sem_t* items, sem_t* empty) {
    sem_wait(items);
    pthread_mutex_lock(queue_mutex);
    
    client_pipes_t c = register_queue->pipe_data[register_queue->head];
    register_queue->head = (register_queue->head + 1) % QUEUE_SIZE;

    pthread_mutex_unlock(queue_mutex);
    sem_post(empty);  
    return c;
}

void write_top5_points(session_thread_arg_t* session_args, int max_games) {
    int top5_file = open("top5.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (top5_file < 0) {
        return;
    }
    int top[5][2] = {{0, -1}, {0, -1}, {0, -1}, {0, -1}, {0, -1}};
    for(int i = 0; i < max_games; i++) {
        int score = 0;
        int id = -1;
        
        pthread_mutex_lock(session_args[i].session_mutex);
        score = session_args[i].points;
        id = session_args[i].client_id;
        pthread_mutex_unlock(session_args[i].session_mutex);
        for(int j = 0; j < 5; j++) {
            if (score > top[j][0] || (top[j][1] != -1 && 0 == top[j][0])) {
                for(int k = 4; k > j; k--) {
                    top[k][0] = top[k-1][0];
                    top[k][1] = top[k-1][1];
                }
                top[j][0] = score;
                top[j][1] = id;
                break;
            }
        }
        
    }
    char buffer[32];
    for(int j = 0; j < 5; j++) {
        if (top[j][1] != -1) {
            snprintf(buffer,sizeof(buffer), "Client %d: %d points\n", top[j][1], top[j][0]);
            write(top5_file, buffer, strlen(buffer));
        }
    }
    if(close(top5_file) < 0){
        perror("close top5.txt");
        return;
    }
    return;
}

int main(int argc, char** argv) {
    if ( argc < 4) {
        fprintf(stderr,
            "Usage: %s <levels_dir> <max_games> <nome_do_FIFO_de_registo>\n",
            argv[0]);
        return 1;
    }
    struct sigaction sa;
    sa.sa_handler = handle_sigusr1;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGUSR1, &sa, NULL);
    // Random seed for any random movements
    srand((unsigned int)time(NULL));
    open_debug_file("debug_server.log");

    DIR* level_dir = opendir(argv[1]);
    if (level_dir == NULL) {
        perror("opendir");
        return -1;
    }
    int total_levels = count_levels(level_dir);

    closedir(level_dir);

    int max_games = atoi(argv[2]);

    register_queue_t* client_queue = malloc(sizeof(register_queue_t));
    pthread_mutex_t queue_mutex;
    sem_t items, empty;    

    queue_init(client_queue, &queue_mutex, &items, &empty);

    pthread_t* sessions = malloc(sizeof(pthread_t) * max_games);
    session_thread_arg_t* session_args = malloc(sizeof(session_thread_arg_t)* max_games);

    


    for (int id_thread = 0; id_thread < max_games; id_thread++) {
        session_args[id_thread].session_mutex = malloc(sizeof(pthread_mutex_t));
        pthread_mutex_init(session_args[id_thread].session_mutex, NULL);
        session_args[id_thread].level_dir_name = argv[1];
        session_args[id_thread].total_levels = total_levels;
        session_args[id_thread].client_queue = client_queue;
        session_args[id_thread].queue_mutex = &queue_mutex;
        session_args[id_thread].items = &items;
        session_args[id_thread].empty = &empty;
        session_args[id_thread].points = 0;
        session_args[id_thread].client_id = -1;

        pthread_create(&sessions[id_thread], NULL, individual_session_thread, &session_args[id_thread]); //os jogos começam todos 
    }

    char* register_pipe_name = argv[3];

    if (level_dir == NULL) {
        fprintf(stderr, "Failed to open directory: %s\n", argv[1]);
        return 0;
    }

    unlink(register_pipe_name); // Unlink existing pipe

    if(mkfifo(register_pipe_name, 0666) == -1){
        perror("mkfifo");
        return 1;  
    }


    int register_pipe_fd = open(register_pipe_name, O_RDWR);
    if(register_pipe_fd < 0){
        perror("open register fifo");
        return 1;
    }

    while(1){    
        if (sigusr1_received) {
            write_top5_points(session_args, max_games);
            sigusr1_received = 0;
        }
        char buffer[81];
        errno = 0;
        ssize_t n = read_full(register_pipe_fd, buffer, 81);
        if (n < 0) {
            perror("read register fifo");
            continue; // erro de leitura → continua vivo
        }

        if (n == 0) {
            // Não deveria acontecer com O_RDWR, mas por segurança:
            continue;
        }

        char op_code = buffer[0];
        if(op_code != (char)('0' + OP_CODE_CONNECT)){
            perror("op_code");
            return 1;
        }

        char client_request_pipe[41];
        char client_notification_pipe[41];

        memcpy(client_request_pipe, buffer + 1, 40);
        client_request_pipe[40] = '\0';

        memcpy(client_notification_pipe, buffer + 41, 40);
        client_notification_pipe[40] = '\0';

        enqueue(client_queue, &queue_mutex, &items, &empty, client_request_pipe, client_notification_pipe);
    }

    close(register_pipe_fd);
    
    for (int id_thread = 0; id_thread < max_games; id_thread++) {
        pthread_join(sessions[id_thread], NULL);
        pthread_mutex_destroy(session_args[id_thread].session_mutex);
        free(session_args[id_thread].session_mutex);
        
    }
    
    free(client_queue);

    close_debug_file();

    close(register_pipe_fd);
    free(sessions);
    free(session_args);

    unlink(register_pipe_name);

    return 0;
}
