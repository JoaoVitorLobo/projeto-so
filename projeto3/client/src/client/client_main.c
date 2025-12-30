#include "api.h"
#include "protocol.h"
#include "display.h"
#include "debug.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <pthread.h>
#include <stdbool.h>
#include <unistd.h>


Board board;
bool stop_execution = false;
int tempo;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

static void *receiver_thread(void *arg) {
    (void)arg;

    debug("RECEIVER THREAD STARTED\n");


    while (true) {
        
        Board board = receive_board_update();

        if (!board.data){
            pthread_mutex_lock(&mutex);
            stop_execution = true;
            pthread_mutex_unlock(&mutex);
            break;
        }
        pthread_mutex_lock(&mutex);
        tempo = board.tempo;
        pthread_mutex_unlock(&mutex);


        draw_board_client(board);
        refresh_screen();

        if (board.game_over == 1 || board.victory == 1) {
            pthread_mutex_lock(&mutex);
            stop_execution = true;
            pthread_mutex_unlock(&mutex);
            break;
        }

        free(board.data);
    }

    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc != 3 && argc != 4) {
        fprintf(stderr,
            "Usage: %s <client_id> <register_pipe> [commands_file]\n",
            argv[0]);
        return 1;
    }

    const char *client_id = argv[1];
    const char *register_pipe = argv[2];
    const char *commands_file = (argc == 4) ? argv[3] : NULL;

    FILE *cmd_fp = NULL;
    if (commands_file) {
        cmd_fp = fopen(commands_file, "r");
        if (!cmd_fp) {
            perror("Failed to open commands file");
            return 1;
        }
    }

    char req_pipe_path[MAX_PIPE_PATH_LENGTH];
    char notif_pipe_path[MAX_PIPE_PATH_LENGTH];

    snprintf(req_pipe_path, MAX_PIPE_PATH_LENGTH,
             "/tmp/%s_request", client_id);

    snprintf(notif_pipe_path, MAX_PIPE_PATH_LENGTH,
             "/tmp/%s_notification", client_id);

    open_debug_file("client_debug.log");

    debug("BEFORE CONNECT\n");

    if (pacman_connect(*client_id, req_pipe_path, notif_pipe_path, register_pipe) != 0) {
        perror("Failed to connect to server");
        return 1;
    }
    debug("AFTER CONNECT\n");

    pthread_t receiver_thread_id;
    pthread_create(&receiver_thread_id, NULL, receiver_thread, NULL);
    debug("Created receiver thread\n");
    terminal_init();
    set_timeout(500);
    draw_board_client(board);
    refresh_screen();

    char command;
    int ch;

    while (1) {
        //debug("Main loop iteration\n");

        pthread_mutex_lock(&mutex);
        if (stop_execution){
            pthread_mutex_unlock(&mutex);
            break;
        }
        pthread_mutex_unlock(&mutex);
        
        if (cmd_fp) {
            // Input from file
            ch = fgetc(cmd_fp);

            if (ch == EOF) {
                // Restart at the start of the file
                rewind(cmd_fp);
                continue;
            }

            command = (char)ch;

            if (command == '\n' || command == '\r' || command == '\0')
                continue;

            command = toupper(command);
            
            // Wait for tempo, to not overflow pipe with requests
            pthread_mutex_lock(&mutex);
            int wait_for = tempo;
            pthread_mutex_unlock(&mutex);

            sleep_ms(wait_for);
            
        } else {
            // Interactive input
            command = get_input();
            command = toupper(command);
        }

        if (command == '\0')
            continue;

        if (command == 'Q') {
            debug("Client pressed 'Q', quitting game\n");
            break;
        }

        debug("Command: %c\n", command);

        debug("Sending command to server: %c\n", command);
        pacman_play(command);

    }
    sleep_ms(1000);
    terminal_cleanup();

    pacman_disconnect();

    pthread_join(receiver_thread_id, NULL);

    if (cmd_fp)
        fclose(cmd_fp);

    pthread_mutex_destroy(&mutex);
    debug("Cleaning up terminal\n");
    terminal_cleanup();
    close_debug_file();

    return 0;
}
