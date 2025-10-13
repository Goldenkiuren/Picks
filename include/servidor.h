// include/servidor.h
#ifndef SERVIDOR_H
#define SERVIDOR_H

#include <stdint.h>
#include "state.h" // para BankState

#define SERVER_PORT_DEFAULT 4000

// Inicializa socket UDP e retorna fd
int server_socket_init(int port);

// Loop principal — recebe pacotes e invoca a lógica
void server_main_loop(int sockfd, BankState *state);

#endif // SERVIDOR_H
