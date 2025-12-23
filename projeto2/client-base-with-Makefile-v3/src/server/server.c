#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <pthread.h>
#include <stdbool.h>
#include <unistd.h>

#include "protocol.h"
#include "api.h"
#include "board.h"
#include "parser.h"


int main(int argc, char *argv[]){
    char const* server_pipe = argv[3];
    int max_games = atoi(argv[2]);
    Board* boards = malloc(sizeof(Board) * max_games); // Placeholder for multiple boards
    int game_count = 0;

    if (argc < 4) {
        fprintf(stderr,
            "Usage: %s <register_pipe> <game_pipe>\n",
            argv[0]);
        return 1;
    }

    unlink(server_pipe); // Unlink existing pipe
    if(mkfifo(server_pipe, 0666) <0){
        perror("mkfifo");
        return 1;
    }
    open(server_pipe, "O_RDONLY"); // Open pipe for reading
    while(1){
        if(game_count >= max_games){
            continue;
        }
        //read(server_pipe, ...); - Read connection request
        // estabelecer a sessão criando os named pipes - mkfifo
        //pacman_connect(, , server_pipe);
        // o cliente está ligado e deve ler o o pipe de notificações para receber a atualizações do tabuleiro
        // o cliente envia comandos através do pipe de pedidos ao servidor para ele processar
        
    }
}