#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include "common.h"

#define BUFFER_SIZE 1024
#define MAX_CLIENTS 500
#define INITIAL_BALANCE 100

//estrutura dos clientes
typedef struct {
    struct sockaddr_in addr;
    uint32_t last_req;
    uint32_t balance;
} ClientInfo;

//tabela de clientes
ClientInfo client_table[MAX_CLIENTS];
int num_clients = 0;
pthread_mutex_t table_mutex = PTHREAD_MUTEX_INITIALIZER;

//funcao de busca ou adicao de cliente
int lookup_or_add_client(struct sockaddr_in* client_addr) {
    //secao critica
    pthread_mutex_lock(&table_mutex);

    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(client_addr->sin_addr), client_ip, INET_ADDRSTRLEN);

    //procura clientes
    for (int i = 0; i < num_clients; i++) {
        char table_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(client_table[i].addr.sin_addr), table_ip, INET_ADDRSTRLEN);
        
        //comparacao de ip
        if (strcmp(client_ip, table_ip) == 0) {
            printf("Cliente %s ja registrado.\n", client_ip);
            client_table[i].addr = *client_addr;
            pthread_mutex_unlock(&table_mutex);
            return i;
        }
    }

    //adiciona cliente se nao encontrado
    if (num_clients < MAX_CLIENTS) {
        int new_index = num_clients;
        client_table[new_index].addr = *client_addr;
        client_table[new_index].last_req = 0;
        client_table[new_index].balance = INITIAL_BALANCE;
        
        printf("Novo cliente registrado: %s. Saldo inicial: %d\n", client_ip, INITIAL_BALANCE);
        
        num_clients++;
        pthread_mutex_unlock(&table_mutex);
        return new_index;
    } else {
        fprintf(stderr, "Tabela de clientes cheia. Novo cliente %s rejeitado.\n", client_ip);
        pthread_mutex_unlock(&table_mutex);
        return -1;
    }
}

int main(int argc, char *argv[]) {

    if (argc != 2) {
        fprintf(stderr, "Use: ./servidor <porta>");
        return 1;
    }

    int port = atoi(argv[1]);
    int sockfd;
    struct sockaddr_in server_addr, client_addr;
    packet pkt;

    //cria socket UDP
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("falha em criar o socket");
        exit(EXIT_FAILURE);
    }

    //zera estruturas de endereço
    memset(&server_addr, 0, sizeof(server_addr));
    memset(&client_addr, 0, sizeof(client_addr));

    //configura endereço do servidor
    server_addr.sin_family = AF_INET; //IPv4
    server_addr.sin_addr.s_addr = INADDR_ANY; //escuta em todas as interfaces de rede disponíveis
    server_addr.sin_port = htons(port); //converte a porta para o formato de rede

    // vincula o socket ao endereço e porta
    if (bind(sockfd, (const struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("falha no bind");
        exit(EXIT_FAILURE);
    }

    printf("Servidor UDP escutando na porta %d\n", port);
    printf("YYYY-MM-DD HH:MM:SS num_transactions 0 total transferred 0 total balance 0\n");

    // recebendo mensagens
    while (1) {
        socklen_t len = sizeof(client_addr);
        int n = recvfrom(sockfd, &pkt, sizeof(pkt), 0, (struct sockaddr *)&client_addr, &len);

        if (n > 0) {
            if (pkt.type == TYPE_DESCOBERTA) {
                printf("Pedido de descoberta recebido de %s\n", inet_ntoa(client_addr.sin_addr));
                lookup_or_add_client(&client_addr);

                packet ack_pkt;
                ack_pkt.type = TYPE_ACK_DESCOBERTA;
                
                sendto(sockfd, &ack_pkt, sizeof(packet), 0, (const struct sockaddr *)&client_addr, len);
            }
        }
    }

    close(sockfd);
    return 0;
}