#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include "common.h"

#define BUFFER_SIZE 1024
#define MAX_CLIENTS 100
#define INITIAL_BALANCE 100

//globais do servidor
client_data client_table[MAX_CLIENTS];
int num_clients = 0;
uint32_t num_transactions = 0;
uint32_t total_transferred = 0;
uint32_t total_balance = 0;

//encontra o indice de um cliente na tabela pelo seu endereco
int find_client(struct sockaddr_in* cliaddr) {
    for (int i = 0; i < num_clients; i++) {
        if (client_table[i].client_ip.s_addr == cliaddr->sin_addr.s_addr) {
            return i;
        }
    }
    return -1;
}

//encontra o destino da transferencia
int find_client_ip(struct in_addr ip_addr) {
    for (int i = 0; i < num_clients ; i++) {
        if (client_table[i].client_ip.s_addr == ip_addr.s_addr) {
            return i;
        }
    }
    return -1;
}

//registra um novo cliente
int register_new_client(struct sockaddr_in* cliaddr) {
    if (num_clients < MAX_CLIENTS) {
        int new_client_id = num_clients;
        client_table[new_client_id].client_ip = cliaddr->sin_addr;
        client_table[new_client_id].last_req = 0;
        client_table[new_client_id].balance = INITIAL_BALANCE;

        num_clients++;
        total_balance += INITIAL_BALANCE;

        printf("Novo cliente registrado: %s | Saldo: %d | Total Clientes: %d | Saldo Banco: %u\n", inet_ntoa(cliaddr->sin_addr), INITIAL_BALANCE, num_clients, total_balance);
        return new_client_id;
    }

    return -1;
}

// obtem a data/hora formatada
void get_current_time(char* buffer, size_t buffer_size) {
    time_t now = time(0);
    struct tm *t = localtime(&now);
    strftime(buffer, buffer_size, "%Y-%m-%d %H:%M:%S", t);
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Uso: ./servidor <porta>\n");
        return 1;
    }

    int port = atoi(argv[1]);
    int sockfd;
    struct sockaddr_in server_addr, client_addr;
    packet pkt;

    // setup
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("falha em criar o socket");
        exit(EXIT_FAILURE);
    }
    memset(&server_addr, 0, sizeof(server_addr));
    memset(&client_addr, 0, sizeof(client_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);
    if (bind(sockfd, (const struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("falha no bind");
        exit(EXIT_FAILURE);
    }
    

    char time_str[100];
    get_current_time(time_str, sizeof(time_str));
    printf("%s num_transactions %u total_transferred %u total_balance %u\n", time_str, num_transactions, total_transferred, total_balance);
    printf("Servidor UDP escutando na porta %d\n", port);


    while(1) {
        socklen_t len = sizeof(client_addr);
        int n = recvfrom(sockfd, &pkt, sizeof(packet), 0, (struct sockaddr *)&client_addr, &len);
        

        if (n > 0) {
            //lógica de descoberta
            if (ntohs(pkt.type) == TYPE_DESCOBERTA) {  // Network to Host
                printf("Pedido de descoberta recebido de %s\n", inet_ntoa(client_addr.sin_addr));

                //verifica se o cliente já existe, se não registra
                int client_idx = find_client(&client_addr);
                if (client_idx == -1) {
                    register_new_client(&client_addr);
                }

                packet ack_pkt;
                memset(&ack_pkt, 0, sizeof(packet));  // Inicializa
                ack_pkt.type = htons(TYPE_ACK_DESCOBERTA);  // Host to Network
                sendto(sockfd, &ack_pkt, sizeof(packet), 0, (const struct sockaddr *)&client_addr, len);                
            }

            else if (ntohs(pkt.type) == TYPE_REQ) {  // Network to Host
                uint32_t seqn = ntohl(pkt.seqn);  // Network to Host
                uint32_t value = ntohl(pkt.value);  // Network to Host

                int origin_idx = find_client(&client_addr);
                int dest_idx = find_client_ip(pkt.dest_addr);

                //envio de erro se origem ou destino não encontrados
                if (origin_idx == -1) {
                    printf("Cliente de origem não encontrado: %s\n", inet_ntoa(client_addr.sin_addr));
                    packet error_pkt;
                    memset(&error_pkt, 0, sizeof(packet));
                    error_pkt.type = htons(TYPE_ERROR_REQ);  // Host to Network
                    sendto(sockfd, &error_pkt, sizeof(packet), 0, (const struct sockaddr *)&client_addr, len);
                    continue;
                }

                if (dest_idx == -1) {
                    printf("Cliente de destino não encontrado: %s\n", inet_ntoa(pkt.dest_addr));
                    packet error_pkt;
                    memset(&error_pkt, 0, sizeof(packet));
                    error_pkt.type = htons(TYPE_ERROR_REQ);  // Host to Network
                    sendto(sockfd, &error_pkt, sizeof(packet), 0, (const struct sockaddr *)&client_addr, len);
                    continue;
                }

                uint32_t new_balance = (uint32_t)client_table[origin_idx].balance;  // Cast pra unsigned pro pacote
                
                //verifica se o saldo é suficiente
                if (client_table[origin_idx].balance >= (int32_t)value) {
                    client_table[origin_idx].balance -= (int32_t)value; //debita da origem
                    client_table[dest_idx].balance += (int32_t)value; //credita no destino
                    
                    client_table[origin_idx].last_req = seqn; // atualiza ultimo id
                
                    new_balance = (uint32_t)client_table[origin_idx].balance;

                    //atualiza servidor
                    num_transactions++;
                    total_transferred += value;
                } else {
                    //saldo insuficiente - CORRIGIDO: %d pro balance (int32_t signed)
                    printf("Saldo insuficiente para cliente %s (saldo: %d, value: %u)\n", inet_ntoa(client_addr.sin_addr), client_table[origin_idx].balance, value);
                }
                //envia resposta ack
                packet ack_pkt;
                memset(&ack_pkt, 0, sizeof(packet));  // Inicializa
                ack_pkt.type = htons(TYPE_ACK_REQ);  // Host to Network
                ack_pkt.balance = htonl(new_balance);  // Host to Network
                ack_pkt.seqn = htonl(seqn);  // Host to Network (usando o convertido)
                sendto(sockfd, &ack_pkt, sizeof(packet), 0, (const struct sockaddr *)&client_addr, len);

                char ip_origin[INET_ADDRSTRLEN];
                char ip_dest[INET_ADDRSTRLEN];
                strcpy(ip_origin, inet_ntoa(client_addr.sin_addr));
                strcpy(ip_dest, inet_ntoa(pkt.dest_addr));
                get_current_time(time_str, sizeof(time_str));
                printf("%s client %s id_req %u dest %s value %u num_transactions %u total_transferred %u total_balance %u\n",
                    time_str,
                    ip_origin, 
                    seqn, 
                    ip_dest, 
                    value, 
                    num_transactions, 
                    total_transferred,  
                    total_balance);
            }
            else if(ntohs(pkt.type) == TYPE_ERROR_REQ) {
                printf("Pacote de erro recebido de %s (ignorado)\n", inet_ntoa(client_addr.sin_addr));
            }
            else {
                printf("Tipo de pacote desconhecido (%d) de %s\n", ntohs(pkt.type), inet_ntoa(client_addr.sin_addr));
            }
        }
    }

    close(sockfd);
    return 0;

}
