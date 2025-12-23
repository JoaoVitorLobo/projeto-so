#include "api.h"
#include "protocol.h"
#include "debug.h"

#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>


struct Session {
  int id;
  int req_pipe;
  int notif_pipe;
  char req_pipe_path[MAX_PIPE_PATH_LENGTH + 1];
  char notif_pipe_path[MAX_PIPE_PATH_LENGTH + 1];
};

static struct Session session = {.id = -1};

int pacman_connect(char const *req_pipe_path, char const *notif_pipe_path, char const *server_pipe_path) {
  int id;
  unlink(req_pipe_path); // Unlink existing pipe
  unlink(notif_pipe_path); // Unlink existing pipe
  int mkfifo_req = mkfifo(req_pipe_path, 0666);
  int mkfifo_notif = mkfifo(notif_pipe_path, 0666);
  int mkfifo_server = mkfifo(server_pipe_path, 0666);
  if(mkfifo_req <0){
      perror("mkfifo");
      return 1;
  }
  if(mkfifo_notif <0){
      perror("mkfifo");
      close(mkfifo_req);
      return 1;
  }
  if(mkfifo_server <0){
      perror("mkfifo");
      close(mkfifo_req);
      close(mkfifo_notif);
      return 1;
  }
  int server_pipe = open(server_pipe_path, O_WRONLY);
  if(server_pipe <0){
      perror("open server pipe");
      close(mkfifo_req);
      close(mkfifo_notif);
      return 1;
  }
  
  session.req_pipe = -1; //ainda não abertos
  session.notif_pipe = -1;

  strncpy(session.req_pipe_path, req_pipe_path, MAX_PIPE_PATH_LENGTH);
  session.req_pipe_path[MAX_PIPE_PATH_LENGTH] = '\0';//garantir terminação
  strncpy(session.notif_pipe_path, notif_pipe_path, MAX_PIPE_PATH_LENGTH);
  session.notif_pipe_path[MAX_PIPE_PATH_LENGTH] = '\0';//garantir terminação

  if(write(server_pipe, &session, sizeof(struct Session))!= sizeof(struct Session)){
      perror("write to server pipe");
      close(mkfifo_req);
      close(mkfifo_notif);
      close(mkfifo_server);
      return 1;
  }

  close(server_pipe);

  int notif_pipe = open(notif_pipe_path, O_RDONLY);
  if(notif_pipe <0){  
      perror("open notif pipe");
      close(mkfifo_req);
      close(mkfifo_notif); 
      return 1;
  }
  int req_pipe = open(req_pipe_path, O_WRONLY);
  if(req_pipe <0){
      perror("open req pipe");
      close(mkfifo_req);
      close(mkfifo_notif);
      close(notif_pipe);
      return 1;
  }
  
  if(read(notif_pipe, &id, sizeof(int))!= sizeof(int)){
      perror("read id from notif pipe");
      close(req_pipe);
      close(notif_pipe);
      close(mkfifo_req);
      close(mkfifo_notif);
      return 1;
  }
  session.id = id;
  session.req_pipe = req_pipe;
  session.notif_pipe = notif_pipe;
  return 0;
}

void pacman_play(char command) {
  if(session.req_pipe <0 || session.id < 0){
      perror("req pipe not opened");
      return;
  }
  if(write(session.req_pipe, &command, sizeof(char))!= sizeof(char)){
      perror("write command to req pipe");
      return;
  }

}

int pacman_disconnect() {
  if(session.id < 0 || session.req_pipe < 0 || session.notif_pipe < 0){
      perror("no active session");
      return 1;
  }

  int msg = OP_CODE_DISCONNECT;
  if(write(session.req_pipe, &msg, sizeof(int))!= sizeof(int)){
      fprintf(stderr, "write disconnect command to req pipe");
  }
  
  if (close(session.req_pipe)){
      fprintf(stderr, "close req pipe");
  }
  if(close(session.notif_pipe)){
      fprintf(stderr, "close notif pipe");
  }

  unlink(session.req_pipe_path);
  unlink(session.notif_pipe_path);

  session.id = -1;
  session.req_pipe = -1;
  session.notif_pipe = -1;

  return 0;
}

int readfull(int fd, void* buffer, size_t size){
    size_t total = 0;
    while(total < size){
        int r = read(fd, buffer + total, size - total);
        if(r <=0){
            return r;
        }
        total += r;
    }
    return total;
}

Board receive_board_update(void) {
  BoardHeader new_boardheader;
  Board new_board;

  //initialize new_board
  new_board.data = NULL;
  new_board.width = 0;
  new_board.height = 0;
  new_board.tempo = 0;
  new_board.victory = 0;
  new_board.game_over = 0;
  new_board.accumulated_points = 0;


  if(session.notif_pipe <0){
      perror("notif pipe not opened");
      return new_board;
  }
  
  if(readfull(session.notif_pipe, &new_boardheader, sizeof(BoardHeader)) != sizeof(BoardHeader)){
      perror("read board from notif pipe");
      new_board.data = NULL;
      return new_board;
  }


  new_board.width = new_boardheader.width;
  new_board.height = new_boardheader.height;
  new_board.tempo = new_boardheader.tempo;
  new_board.victory = new_boardheader.victory;
  new_board.game_over = new_boardheader.game_over;
  new_board.accumulated_points = new_boardheader.accumulated_points;
  new_board.data = malloc(new_boardheader.data_size);


  if (!new_board.data) {
    perror("malloc");
    return new_board;
  }
  if(readfull(session.notif_pipe, new_board.data, new_boardheader.data_size) != new_boardheader.data_size){
      perror("read board data from notif pipe");
      free(new_board.data);
      new_board.data = NULL;
      return new_board;
  }
  return new_board;
}