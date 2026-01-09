#include "api.h"
#include "protocol.h"
#include "debug.h"
#include "utils.h"

#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <sys/stat.h>
#include <stdlib.h>


struct Session {
  int id; //id
  int req_pipe_fd;
  int notif_pipe_fd;
  char req_pipe_path[MAX_PIPE_PATH_LENGTH + 1];
  char notif_pipe_path[MAX_PIPE_PATH_LENGTH + 1];
};

static struct Session session = {.id = -1};

int pacman_connect(char const id_client, char const *req_pipe_path, char const *notif_pipe_path, char const *server_pipe_path) {
    session.id = id_client - '0'; //convert char to int

    strncpy(session.req_pipe_path, req_pipe_path, MAX_PIPE_PATH_LENGTH);
    session.req_pipe_path[MAX_PIPE_PATH_LENGTH] = '\0';//garantir terminação
    strncpy(session.notif_pipe_path, notif_pipe_path, MAX_PIPE_PATH_LENGTH);
    session.notif_pipe_path[MAX_PIPE_PATH_LENGTH] = '\0';//garantir terminação

    unlink(req_pipe_path); // Unlink existing pipe
    unlink(notif_pipe_path); // Unlink existing pipe

    mkfifo(req_pipe_path, 0666);
    mkfifo(notif_pipe_path, 0666);

    char absolut_server_pipe_path[MAX_PIPE_PATH_LENGTH *2];
    snprintf(absolut_server_pipe_path, sizeof(absolut_server_pipe_path), "../server/%s", server_pipe_path);
    int server_pipe_fd = open(absolut_server_pipe_path, O_RDWR);

    char message[81];
    message[0] = (char)('0' + OP_CODE_CONNECT);
    memcpy(message + 1, req_pipe_path, 40);
    memcpy(message + 41, notif_pipe_path, 40);

    write_full(server_pipe_fd, message, 81);

    close(server_pipe_fd);

    int notif_pipe_fd = open(notif_pipe_path, O_RDWR);

    session.req_pipe_fd = -1;
    session.notif_pipe_fd = notif_pipe_fd;


    char buffer[2];
    read_full(session.notif_pipe_fd, buffer, 2);
    int op_code = buffer[0] - '0';
    if (op_code != OP_CODE_CONNECT) {
        perror("Invalid op_code received - pacman_connect");
        return -1;
    }
    char result = buffer[1] - '0';

    return result;
}

  void pacman_play(char command) {
    if(session.req_pipe_fd < 0){
        int req_pipe_fd = open(session.req_pipe_path, O_RDWR);
        session.req_pipe_fd = req_pipe_fd;

    }

    char msg[2];
    msg[0] = (char)('0' + OP_CODE_PLAY);
    msg[1] = command;

    if(write_full(session.req_pipe_fd, msg, sizeof(msg))!= sizeof(msg)){
        perror("write command to req pipe");
        return;
    }
}

int pacman_disconnect() {
    if (session.req_pipe_fd < 0) {
        int req_pipe_fd = open(session.req_pipe_path, O_RDWR);
        session.req_pipe_fd = req_pipe_fd;
    }
    int op_code = (char)('0' + OP_CODE_DISCONNECT);
    char msg[1] = {op_code};
    if (write_full(session.req_pipe_fd, msg, sizeof(char)) != sizeof(msg)) {
        perror("write disconnect command to req pipe");
        return 1;
    }
    
    if (close(session.req_pipe_fd)){
        perror("close req pipe");
        return 1;
    }
    if(close(session.notif_pipe_fd)){
        perror("close notif pipe");
        return 1;
    }

    unlink(session.req_pipe_path);
    unlink(session.notif_pipe_path);

    session.id = -1;
    session.req_pipe_fd = -1;
    session.notif_pipe_fd = -1;

    return 0;
}

void read_notification_fifo(Board *new_board){
    char initial_buffer[sizeof(char) + (sizeof(int)*6)];
    ssize_t bytes_read = read_full(session.notif_pipe_fd, initial_buffer, sizeof(char) + (sizeof(int)*6));
    
    if (bytes_read <= 0) {
        return;
    }
    
    size_t ptr = 0;

    int op_code = initial_buffer[ptr] - '0';
    if(op_code != OP_CODE_BOARD){
        perror("Invalid op_code received");
        return;
    }
    ptr += 1;

    memcpy(&(new_board->width), initial_buffer + ptr, sizeof(int));
    ptr += sizeof(int);

    memcpy(&(new_board->height), initial_buffer + ptr, sizeof(int));
    ptr += sizeof(int);

    memcpy(&(new_board->tempo), initial_buffer + ptr, sizeof(int));
    ptr += sizeof(int);

    memcpy(&(new_board->victory), initial_buffer + ptr, sizeof(int));
    ptr += sizeof(int);

    memcpy(&(new_board->game_over), initial_buffer + ptr, sizeof(int));
    ptr += sizeof(int);

    memcpy(&(new_board->accumulated_points), initial_buffer + ptr, sizeof(int));
    ptr += sizeof(int);

    int data_size = sizeof(char) * new_board->width * new_board->height;
    char *data_buffer = (char*) malloc(data_size);
    read_full(session.notif_pipe_fd, data_buffer, data_size);// buffer sendo liberado no client_main.c
    new_board->data = data_buffer;
}

Board receive_board_update(void) {
    //BoardHeader new_boardheader;
    Board new_board = {0};

    if(session.notif_pipe_fd <0){
        perror("notif pipe not opened");
        return new_board;
    }
    
    read_notification_fifo(&new_board);


    return new_board;
}