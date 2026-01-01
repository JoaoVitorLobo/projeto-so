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

#define CONTINUE_PLAY 0
#define NEXT_LEVEL 1
#define QUIT_GAME 2
#define LOAD_BACKUP 3
#define CREATE_BACKUP 4


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
} ghost_thread_arg_t;

typedef struct {
    char *level_dir_name;
    int total_levels;
    register_queue_t *client_queue;
    pthread_mutex_t *queue_mutex;
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
    board_t *board;
    char *client_request_pipe;
} pacman_thread_arg_t;

int thread_shutdown = 0;

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
        debug("[%d] Created\n", getpid());

        return 0;
    }
}

void screen_refresh(board_t * game_board, int mode) {
    //debug("REFRESH\n");
    draw_board(game_board, mode);
    refresh_screen();     
}

void* ncurses_thread(void *arg) {
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

        board_to_message(message, board, *victory, *game_over, *accumulated_points);

        debug("WRITING IN: %d\n", client_notification_fd);
        write_full(client_notification_fd, message, data_size);
    }
    return NULL;
}

void* pacman_thread(void *arg) {
    pacman_thread_arg_t *pacman_arg = (pacman_thread_arg_t*) arg;

    board_t *board = pacman_arg->board;
    char *client_request_pipe = pacman_arg->client_request_pipe;

    pacman_t* pacman = &board->pacmans[0];
    debug("PACMAN THREAD\n");

    int *retval = malloc(sizeof(int));

    int client_request_fd = open(client_request_pipe, O_RDONLY);

    while (true) {
        if(!pacman->alive) {
            *retval = LOAD_BACKUP;
            return (void*) retval;
        }

        sleep_ms(board->tempo * (1 + pacman->passo));

        char buffer[1];
        read_full(client_request_fd, buffer, 1);
        int op_code = buffer[0] - '0';
        debug("OPCODE FROM PLAY %c\n", buffer[0]);
        if(op_code == OP_CODE_DISCONNECT){
            debug("Receiving disconnect message (1 bytes): op=%c\n", buffer[0]);
            *retval = QUIT_GAME;
            close(client_request_fd);
            return (void*) retval;
        }
        read_full(client_request_fd, buffer, 1);
        debug("COMMAND FROM PLAY %c\n", buffer[0]);
        debug("comand read\n");
        char command = buffer[0];
        debug("Receiving play message (2 bytes): op=%d command=%c\n", op_code, command);

        command_t* play;
        command_t c;

        c.command = command;
        c.turns = 1;
        play = &c;


        debug("KEY %c\n", play->command);

        // QUIT
        if (play->command == 'Q') {
            *retval = QUIT_GAME;
            close(client_request_fd);
            return (void*) retval;
        }
        // FORK
        if (play->command == 'G') {
            *retval = CREATE_BACKUP;
            close(client_request_fd);
            return (void*) retval;
        }

        pthread_rwlock_rdlock(&board->state_lock);

        int result = move_pacman(board, 0, play);
        if (result == REACHED_PORTAL) {
            // Next level
            *retval = NEXT_LEVEL;
            break;
        }
        else if(result == DEAD_PACMAN) {
            // Restart from child, wait for child, then quit
            *retval = LOAD_BACKUP;
            break;
        }
        else{
            *retval = CONTINUE_PLAY;
            break;
        }

        pthread_rwlock_unlock(&board->state_lock);
    }
    pthread_rwlock_unlock(&board->state_lock);
    close(client_request_fd);
    return (void*) retval;
}

void* ghost_thread(void *arg) {
    ghost_thread_arg_t *ghost_arg = (ghost_thread_arg_t*) arg;
    board_t *board = ghost_arg->board;
    int ghost_ind = ghost_arg->ghost_index;

    free(ghost_arg);

    ghost_t* ghost = &board->ghosts[ghost_ind];

    while (true) {
        sleep_ms(board->tempo * (1 + ghost->passo));

        pthread_rwlock_rdlock(&board->state_lock);
        if (thread_shutdown) {
            pthread_rwlock_unlock(&board->state_lock);
            pthread_exit(NULL);
        }
        
        move_ghost(board, ghost_ind, &ghost->moves[ghost->current_move%ghost->n_moves]);
        pthread_rwlock_unlock(&board->state_lock);
    }
}

void board_to_message(char *message, board_t* game_board, int victory, int game_over, int accumulated_points) {
    int data_size = sizeof(char) + (sizeof(int)*6) + (sizeof(char)* game_board->width * game_board->height);
    char *ptr = message;

    // op_code (1 byte)
    ptr[0] = (char)('0' + OP_CODE_BOARD); //OP_CODE_BOARD
    ptr += 1;

    // width
    memcpy(ptr, &game_board->width, sizeof(int));
    ptr += sizeof(int);

    // height
    memcpy(ptr, &game_board->height, sizeof(int));
    ptr += sizeof(int);

    // tempo
    memcpy(ptr, &game_board->tempo, sizeof(int));
    ptr += sizeof(int);

    // victory
    int vic = victory;
    memcpy(ptr, &vic, sizeof(int));
    ptr += sizeof(int);

    // game_over
    int eg = game_over;
    memcpy(ptr, &eg, sizeof(int));
    ptr += sizeof(int);

    // accumulated_points
    memcpy(ptr, &accumulated_points, sizeof(int));
    ptr += sizeof(int);

    // board data (width * height bytes)
    //memcpy(ptr, game_board->board, game_board->width * game_board->height);
    for (int i = 0; i < game_board->width * game_board->height; i++) {
        switch(game_board->board[i].content) {
            case 'W':
                ptr[i] = 'X';
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


    debug("Sending update message to notifications (%d bytes): op=%c width=%d height=%d tempo: %d victory: %d game_over: %d accumulated_points: %d\n", data_size, message[0], game_board->width, game_board->height, game_board->tempo, vic, eg, accumulated_points);
    for (int lin = 0; lin < game_board->height; lin++) {
        for (int col = 0; col < game_board->width; col++) {
            debug("%c", game_board->board[lin * game_board->width + col].content);
        }
        debug("\n");
    }
}

void* individual_session_thread(void *session_args) {
    session_thread_arg_t *thread_arg = (session_thread_arg_t *) session_args;

    char *level_dir_name = thread_arg->level_dir_name;
    int total_levels = thread_arg->total_levels;
    register_queue_t* client_queue = thread_arg->client_queue;
    pthread_mutex_t* queue_mutex = thread_arg->queue_mutex;
    sem_t* items = thread_arg->items;
    sem_t* empty = thread_arg->empty;

    DIR* level_dir = opendir(level_dir_name);

    free(thread_arg);

    client_pipes_t client_pipe_data = dequeue(client_queue, queue_mutex, items, empty);
    
    char *client_request_pipe = client_pipe_data.client_request_pipe;
    char *client_notification_pipe = client_pipe_data.client_notification_pipe;

    debug("INDIVIDUAL SESSION THREAD\n");

    char message[2];
    message[0] = (char)('0' + OP_CODE_CONNECT);

    int client_notification_fd = open(client_notification_pipe, O_WRONLY);
    if (client_notification_fd < 0) {
        perror("open client fifo");
        message[1] = '1'; 
        return NULL;
    }
    message[1] = '0'; 
    debug("Sending return message to connect (2 bytes): op=%c result=%c\n", message[0], message[1]);
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
        
            //int data_size = sizeof(char) + (sizeof(int)*6) + (sizeof(char)* game_board.width * game_board.height);
            //char message[data_size];

            //board_to_message(message, &game_board, victory, game_over, accumulated_points);

            //debug("WRITING IN: %d\n", client_notification_fd);
            //write_full(client_notification_fd, message, data_size);

            while(true) {
                pthread_t ncurses_tid, pacman_tid;
                pthread_t *ghost_tids = malloc(game_board.n_ghosts * sizeof(pthread_t));

                thread_shutdown = 0;

                debug("Creating threads\n");

 
                pacman_thread_arg_t* pacman_arg = malloc(sizeof(pacman_thread_arg_t));
                pacman_arg->board = &game_board;
                pacman_arg->client_request_pipe = client_request_pipe;

                pthread_create(&pacman_tid, NULL, pacman_thread, pacman_arg);
                debug("Created pacman thread\n");
                for (int i = 0; i < game_board.n_ghosts; i++) {
                    ghost_thread_arg_t *arg = malloc(sizeof(ghost_thread_arg_t));
                    arg->board = &game_board;
                    arg->ghost_index = i;
                    pthread_create(&ghost_tids[i], NULL, ghost_thread, (void*) arg);
                }
                debug("Created ghost threads\n");
                
                game_board.session_active = true;

                ncurses_thread_arg_t *ncurses_arg = malloc(sizeof(ncurses_thread_arg_t));
                if (ncurses_arg == NULL) {
                    perror("malloc ncurses_arg");
                    thread_shutdown = 1;
                    break;
                }
                ncurses_arg->board = &game_board;
                ncurses_arg->victory = &victory;
                ncurses_arg->game_over = &game_over;
                ncurses_arg->accumulated_points = &accumulated_points;
                ncurses_arg->client_notification_fd = client_notification_fd;
                pthread_create(&ncurses_tid, NULL, ncurses_thread, ncurses_arg);


                int *retval;
                pthread_join(pacman_tid, (void**)&retval); // ele não pode ficar à espera do pacman acabar, pois assim só dá refresh quando acaba/troca de nivel
                debug("Pacman thread joined\n");

                pthread_rwlock_wrlock(&game_board.state_lock);
                thread_shutdown = 1;
                pthread_rwlock_unlock(&game_board.state_lock);

                for (int i = 0; i < game_board.n_ghosts; i++) {
                    pthread_join(ghost_tids[i], NULL);
                }

                game_board.session_active = false;
                pthread_join(ncurses_tid, NULL);

                debug("Ghost threads joined\n");

                free(ghost_tids);
                free(pacman_arg);

                int result = *retval;
                free(retval);

                if(result == NEXT_LEVEL) {
                    screen_refresh(&game_board, DRAW_WIN);
                    sleep_ms(game_board.tempo);
                    if (current_level >= total_levels) {
                        debug("All levels completed. Victory! current_level=%d total_levels=%d\n", current_level, total_levels);
                        end_game = 1;
                        debug("victory = 1\n");
                        victory = 1;
                    }
                    debug("returned-5\n");
                    break;
                }

                if(result == CREATE_BACKUP) {
                    debug("CREATE_BACKUP\n");
                    if (parent_process == getpid()) {
                        debug("PARENT\n");
                        pid_t child = create_backup();
                        if (child == -1) {
                            // failed to fork
                            debug("[%d] Failed to create backup\n", getpid());
                            end_game = true;
                            debug("returned-4\n");
                            break;
                        }
                        if (child > 0) {
                            debug("Parent process\n");
                            int status;
                            wait(&status);

                            if (WIFEXITED(status)) {
                                int code = WEXITSTATUS(status);
                                
                                if (code == 1) {
                                    terminal_init();
                                    debug("[%d] Save Resuming...\n", getpid());
                                }
                                else { // End game or error
                                    end_game = true;
                                    debug("returned-3\n");
                                    break;
                                }
                            }
                        } else {
                            terminal_init();
                            debug("Child process\n");
                        }

                    } else {
                        debug("[%d] Only parent process can have a save\n", getpid());
                    }
                }

                if(result == LOAD_BACKUP) {
                    if(getpid() != parent_process) {
                        terminal_cleanup();
                        unload_level(&game_board);
                        
                        close_debug_file();

                        if (closedir(level_dir) == -1) {
                            fprintf(stderr, "Failed to close directory\n");
                            debug("returned-2\n");
                            return NULL;
                        }
                        debug("returned-1\n");
                        return NULL;
                    } else {
                        // No backup process, game over
                        result = QUIT_GAME;
                    }
                }

                if(result == QUIT_GAME) {
                    screen_refresh(&game_board, DRAW_GAME_OVER); 
                    sleep_ms(game_board.tempo);
                    debug("game over = 1\n");
                    game_over = 1;
                    end_game = true;
                    debug("QUIT_GAME\n");
                    break;
                }
                
                accumulated_points = game_board.pacmans[0].points;
                debug("Accumulated points: %d\n", accumulated_points);

            }
            int data_size = sizeof(char) + (sizeof(int)*6) + (sizeof(char)* game_board.width * game_board.height);
            char message[data_size];

            board_to_message(message, &game_board, victory, game_over, accumulated_points);

            debug("WRITING IN: %d\n", client_notification_fd);
            write_full(client_notification_fd, message, data_size);
            
            unload_level(&game_board);
        }
    }   

    close(client_notification_fd);
    closedir(level_dir);
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
    
    debug("Enqueuing client pipes: req=%s, notif=%s\n", req, notif);
    debug("new queue:");
    for (int i = 0; i < register_queue->tail; i++) {
        debug(" [%d]: req=%s, notif=%s;", i, register_queue->pipe_data[i].client_request_pipe, register_queue->pipe_data[i].client_notification_pipe);
    }
    debug("\n");
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

int main(int argc, char** argv) {
    if ( argc < 4) {
        fprintf(stderr,
            "Usage: %s <levels_dir> <max_games> <nome_do_FIFO_de_registo>\n",
            argv[0]);
        return 1;
    }

    // Random seed for any random movements
    srand((unsigned int)time(NULL));
    open_debug_file("debug_server.log");

    debug("opening level dir: %s\n", argv[1]);
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


    for (int id_thread = 0; id_thread < max_games; id_thread++) {
        session_thread_arg_t* session_args = malloc(sizeof(session_thread_arg_t));
        session_args->level_dir_name = argv[1];
        session_args->total_levels = total_levels;
        session_args->client_queue = client_queue;
        session_args->queue_mutex = &queue_mutex;
        session_args->items = &items;
        session_args->empty = &empty;

        debug("BEFORE Creating session manager thread\n");
        pthread_create(&sessions[id_thread], NULL, individual_session_thread, session_args); //os jogos começam todos 
    }

    char* register_pipe_name = argv[3];

    if (level_dir == NULL) {
        fprintf(stderr, "Failed to open directory: %s\n", argv[1]);
        return 0;
    }

    debug("unlink fifo: %s\n", register_pipe_name);
    unlink(register_pipe_name); // Unlink existing pipe
    debug("Creating register fifo: %s\n", register_pipe_name);
    if(mkfifo(register_pipe_name, 0666) == -1){
        perror("mkfifo");
        return 1;  
    }


    int register_pipe_fd;

    while(1){    
        register_pipe_fd = open(register_pipe_name, O_RDONLY);
        if(register_pipe_fd < 0){
            perror("open register fifo");
            return 1;
        }
        char buffer[81];
        ssize_t n = read_full(register_pipe_fd, buffer, 81);
        debug("read from register fifo: %s\n", buffer);
        if (n <= 0) break; 

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

        debug("Enqueuing client pipes: req=%s, notif=%s\n", client_request_pipe, client_notification_pipe);
        enqueue(client_queue, &queue_mutex, &items, &empty, client_request_pipe, client_notification_pipe);
        close(register_pipe_fd);
    }
    
    for (int id_thread = 0; id_thread < max_games; id_thread++) {
        pthread_join(sessions[id_thread], NULL);
    }

    free(client_queue);

    close_debug_file();

    close(register_pipe_fd);


    unlink(register_pipe_name);

    return 0;
}
