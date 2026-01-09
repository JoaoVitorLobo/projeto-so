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
            free(board.data);
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

    if (pacman_connect(*client_id, req_pipe_path, notif_pipe_path, register_pipe) != 0) {
        perror("Failed to connect to server");
        return 1;
    }

    pthread_t receiver_thread_id;
    pthread_create(&receiver_thread_id, NULL, receiver_thread, NULL);

    terminal_init();
    set_timeout(500);
    draw_board_client(board);
    refresh_screen();

    char command;
    char buffer[4096];
    int curr_command_index = 0;
    char* commands = (char*) malloc(sizeof(char));
    int n_commands = 0;

    if (cmd_fp){
        while(read_line(fileno(cmd_fp), buffer) != 0){
            if (strncmp(buffer, "#", 1) == 0 || strncmp(buffer, "P", 1) == 0 || strncmp(buffer, "\n", 1) == 0){
                continue;
            } 
            else {
                commands = (char*) realloc(commands, (n_commands + 1) * sizeof(char));
                commands[n_commands] = buffer[0];
                n_commands++;
            }
        }
    }

    while (1) {
        pthread_mutex_lock(&mutex);
        if (stop_execution){
            pthread_mutex_unlock(&mutex);
            break;
        }
        pthread_mutex_unlock(&mutex);
        
        if (cmd_fp) {
            command = commands[curr_command_index % n_commands];
            curr_command_index++;    
            
            // Wait for tempo, to not overflow pipe with requests
            pthread_mutex_lock(&mutex);
            int wait_for = tempo;
            pthread_mutex_unlock(&mutex);

            sleep_ms(wait_for);
        } 
        else {
            // Interactive input
            command = get_input();
            command = toupper(command);
        }
        if (command == '\0') {
            continue; // No input, continue the loop
        }

        if (command == 'Q') {
            break;
        }
        pacman_play(command);

    }
    free(commands);

    sleep_ms(1000);

    terminal_cleanup();

    pacman_disconnect();

    pthread_join(receiver_thread_id, NULL);

    if (cmd_fp)
        fclose(cmd_fp);

    pthread_mutex_destroy(&mutex);
    terminal_cleanup();
    close_debug_file();

    return 0;
}
