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
  int confirm;
  unlink(req_pipe_path); // Unlink existing pipe
  unlink(notif_pipe_path); // Unlink existing pipe
  if(mkfifo(req_pipe_path, 0666) <0){
      perror("mkfifo");
      return 1;
  }
  if(mkfifo(notif_pipe_path, 0666) <0){
      perror("mkfifo");
      close(req_pipe_path);
      return 1;
  }
  int server_pipe = open(server_pipe_path, O_WRONLY);
  if(server_pipe <0){
      perror("open server pipe");
      close(req_pipe_path);
      close(notif_pipe_path);
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
      close(req_pipe_path);
      close(notif_pipe_path);
      close(server_pipe);
      return 1;
  }
  close(server_pipe);

  int notif_pipe = open(notif_pipe_path, O_RDONLY);
  if(notif_pipe <0){  
      perror("open notif pipe");
      close(req_pipe_path);
      close(notif_pipe_path); 
      return 1;
  }
  int req_pipe = open(req_pipe_path, O_WRONLY);
  if(req_pipe <0){
      perror("open req pipe");
      close(req_pipe_path);
      close(notif_pipe_path);
      close(notif_pipe);
      return 1;
  }
  
  if(read(notif_pipe, &id, sizeof(int))!= sizeof(int)){
      perror("read id from notif pipe");
      close(req_pipe);
      close(notif_pipe);
      close(req_pipe_path);
      close(notif_pipe_path);
      return 1;
  }
  session.id = id;
  session.req_pipe = req_pipe;
  session.notif_pipe = notif_pipe;
  return 0;
}

void pacman_play(char command) {

  // TODO - implement me

}

int pacman_disconnect() {
  // TODO - implement me
  return 0;
}

Board receive_board_update(void) {
    // TODO - implement me
}