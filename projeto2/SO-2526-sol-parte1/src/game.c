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

#define CONTINUE_PLAY 0
#define NEXT_LEVEL 1
#define QUIT_GAME 2
#define LOAD_BACKUP 3
#define CREATE_BACKUP 4

typedef struct {
    board_t *board;
    int ghost_index;
} ghost_thread_arg_t;

typedef struct {
    char *level_dir_name;
    DIR* level_dir;
    char *client_request_pipe;
    char *client_notification_pipe;
} session_thread_arg_t;

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
    debug("REFRESH\n");
    draw_board(game_board, mode);
    refresh_screen();     
}

void* ncurses_thread(void *arg) {
    board_t *board = (board_t*) arg;
    sleep_ms(board->tempo / 2);
    while (true) {
        sleep_ms(board->tempo);
        pthread_rwlock_wrlock(&board->state_lock);
        if (thread_shutdown) {
            pthread_rwlock_unlock(&board->state_lock);
            pthread_exit(NULL);
        }
        screen_refresh(board, DRAW_MENU);
        pthread_rwlock_unlock(&board->state_lock);
    }
}

void* pacman_thread(void *arg) {
    pacman_thread_arg_t *pacman_arg = (pacman_thread_arg_t*) arg;

    board_t *board = pacman_arg->board;
    char *client_request_pipe = pacman_arg->client_request_pipe;

    pacman_t* pacman = &board->pacmans[0];

    int *retval = malloc(sizeof(int));

    while (true) {
        if(!pacman->alive) {
            *retval = LOAD_BACKUP;
            return (void*) retval;
        }

        sleep_ms(board->tempo * (1 + pacman->passo));

        int client_request_fd = open(client_request_pipe, O_RDONLY);
        char buffer[1];
        read_full(client_request_fd, buffer, 1);
        int op_code = buffer[0];
        if(op_code == OP_CODE_DISCONNECT){
            *retval = QUIT_GAME;
            return (void*) retval;
        }
        read_full(client_request_fd, buffer, 1);
        char command = buffer[0];

        command_t* play;
        command_t c;

        c.command = command;
        c.turns = 1;
        play = &c;


        debug("KEY %c\n", play->command);

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
        if (result == REACHED_PORTAL) {
            // Next level
            *retval = NEXT_LEVEL;
            break;
        }

        if(result == DEAD_PACMAN) {
            // Restart from child, wait for child, then quit
            *retval = LOAD_BACKUP;
            break;
        }

        pthread_rwlock_unlock(&board->state_lock);
    }
    pthread_rwlock_unlock(&board->state_lock);
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

void* individual_session_thread(void *session_args) {
    session_thread_arg_t *thread_arg = (session_thread_arg_t *) session_args;

    char *level_dir_name = thread_arg->level_dir_name;
    DIR* level_dir = thread_arg->level_dir;
    char *client_request_pipe = thread_arg->client_request_pipe;
    char *client_notification_pipe = thread_arg->client_notification_pipe;

    
    int accumulated_points = 0;
    bool end_game = false;
    board_t game_board;

    pid_t parent_process = getpid(); // Only the parent process can create backups

    struct dirent* entry;
    while ((entry = readdir(level_dir)) != NULL && !end_game) {
        if (entry->d_name[0] == '.') continue;

        char *dot = strrchr(entry->d_name, '.');
        if (!dot) continue;

        if (strcmp(dot, ".lvl") == 0) {
            load_level(&game_board, entry->d_name, level_dir_name, accumulated_points);
            while(true) {
                pthread_t /*ncurses_tid,*/ pacman_tid;
                pthread_t *ghost_tids = malloc(game_board.n_ghosts * sizeof(pthread_t));

                thread_shutdown = 0;

                debug("Creating threads\n");

 
                pacman_thread_arg_t* pacman_arg = malloc(sizeof(pacman_thread_arg_t));
                pacman_arg->board = &game_board;
                pacman_arg->client_request_pipe = client_request_pipe;

                pthread_create(&pacman_tid, NULL, pacman_thread, pacman_arg);
                for (int i = 0; i < game_board.n_ghosts; i++) {
                    ghost_thread_arg_t *arg = malloc(sizeof(ghost_thread_arg_t));
                    arg->board = &game_board;
                    arg->ghost_index = i;
                    pthread_create(&ghost_tids[i], NULL, ghost_thread, (void*) arg);
                }
                //pthread_create(&ncurses_tid, NULL, ncurses_thread, (void*) &game_board);

                int *retval;
                pthread_join(pacman_tid, (void**)&retval);

                pthread_rwlock_wrlock(&game_board.state_lock);
                thread_shutdown = 1;
                pthread_rwlock_unlock(&game_board.state_lock);

                //pthread_join(ncurses_tid, NULL);
                for (int i = 0; i < game_board.n_ghosts; i++) {
                    pthread_join(ghost_tids[i], NULL);
                }

                free(ghost_tids);
                free(pacman_arg);

                int result = *retval;
                free(retval);

                if(result == NEXT_LEVEL) {
                    screen_refresh(&game_board, DRAW_WIN);
                    sleep_ms(game_board.tempo);
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
                            return NULL;
                        }

                        return NULL;
                    } else {
                        // No backup process, game over
                        result = QUIT_GAME;
                    }
                }

                if(result == QUIT_GAME) {
                    screen_refresh(&game_board, DRAW_GAME_OVER); 
                    sleep_ms(game_board.tempo);
                    end_game = true;
                    break;
                }

                accumulated_points = game_board.pacmans[0].points;
                
                int data_size = sizeof(char) + (sizeof(int)*6) + (sizeof(char)* game_board.width * game_board.height);
                char message[data_size];
                char *ptr = message;

                // op_code (1 byte)
                *ptr = OP_CODE_BOARD; //OP_CODE_BOARD
                ptr += 1;


                // width
                memcpy(ptr, &game_board.width, sizeof(int));
                ptr += sizeof(int);

                // height
                memcpy(ptr, &game_board.height, sizeof(int));
                ptr += sizeof(int);

                // tempo
                memcpy(ptr, &game_board.tempo, sizeof(int));
                ptr += sizeof(int);

                // victory
                int vic = end_game ? 1 : 0;
                memcpy(ptr, &vic, sizeof(int));
                ptr += sizeof(int);

                // end_game
                int eg = end_game ? 1 : 0;
                memcpy(ptr, &eg, sizeof(int));
                ptr += sizeof(int);

                // accumulated_points
                memcpy(ptr, &accumulated_points, sizeof(int));
                ptr += sizeof(int);

                // board data (width * height bytes)
                memcpy(ptr, game_board.board, game_board.width * game_board.height);

                int client_notification_fd = open(client_notification_pipe, O_WRONLY);
                if (client_notification_fd < 0) {
                    perror("open client fifo");
                    return NULL;
                }
                write(client_notification_fd, message, data_size);
                close(client_notification_fd);
            }
            unload_level(&game_board);
        }
    }    

    return NULL;
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

    //int max_games = atoi(argv[2]);
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

    debug("Opening register fifo: %s\n", register_pipe_name);
    int register_pipe_fd = open(register_pipe_name, O_RDONLY);
    debug("Opened register fifo: %s\n", register_pipe_name);
    char buffer[81];
    //int n_bytes_read = 0;
    debug("BEFORE Reading register fifo: %s\n", register_pipe_name);
    read(register_pipe_fd, buffer, 81);
    debug("AFTER Read from register fifo: %s\n", buffer);

    char op_code = buffer[0];
    if(op_code != '1'){
        perror("op_code");
        return 1;
    }
    char client_request_pipe[41];
    char client_notification_pipe[41];

    memcpy(client_request_pipe, buffer + 1, 40);
    client_request_pipe[40] = '\0';

    memcpy(client_notification_pipe, buffer + 41, 40);
    client_notification_pipe[40] = '\0';

        
    open_debug_file("debug.log");


    

    session_thread_arg_t* session_args = malloc(sizeof(session_thread_arg_t));
    session_args->level_dir_name = argv[1];
    session_args->level_dir = level_dir;
    session_args->client_request_pipe = client_request_pipe;
    session_args->client_notification_pipe = client_notification_pipe;


    pthread_t session_manager_thread;
    pthread_create(&session_manager_thread, NULL, individual_session_thread, session_args);
    
    pthread_join(session_manager_thread, NULL);

    close_debug_file();

    close(register_pipe_fd);

    if (closedir(level_dir) == -1) {
        fprintf(stderr, "Failed to close directory\n");
        return 0;
    }

    unlink(register_pipe_name);
    free(session_args);

    return 0;
}
