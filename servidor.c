#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
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

        printf("Novo cliente registrado: %s | Saldo: %d | Total Clientes: %d | Saldo Banco: %d\n", inet_ntoa(cliaddr->sin_addr), INITIAL_BALANCE, num_clients, total_balance);
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
            if (pkt.type == TYPE_DESCOBERTA) {
                printf("Pedido de descoberta recebido de %s\n", inet_ntoa(client_addr.sin_addr));

                //verifica se o cliente já existe, se não registra
                int client_idx = find_client(&client_addr);
                if (client_idx == -1) {
                    register_new_client(&client_addr);
                }

                packet ack_pkt;
                ack_pkt.type = TYPE_ACK_DESCOBERTA;
                sendto(sockfd, &ack_pkt, sizeof(packet), 0, (const struct sockaddr *)&client_addr, len);                
            }

            else if (pkt.type == TYPE_REQ) {
                int origin_idx = find_client(&client_addr);
                int dest_idx = find_client_ip(pkt.dest_addr);

                int32_t new_balance = -1; //saldo a ser retornado

                //verifica se a origem e o destino existem
                if (origin_idx != -1 && dest_idx != -1) {
                    if (client_table[origin_idx].balance >= pkt.value) {
                        client_table[origin_idx].balance -= pkt.value; //debita da origem
                        client_table[dest_idx].balance += pkt.value; //credita no destino
                        
                        client_table[origin_idx].last_req = pkt.seqn; // atualiza ultimo id
                    
                        new_balance = client_table[origin_idx].balance;

                        //atualiza servidor
                        num_transactions++;
                        total_transferred += pkt.value;
                    } else {
                        //saldo insuficiente
                        printf("Saldo insuficiente para cliente %s\n", inet_ntoa(client_addr.sin_addr));
                        new_balance = client_table[origin_idx].balance; //saldo atual
                    }
                } else {
                    // cliente origem ou destino não encontrado
                    printf("Cliente de origem ou destino não encontrado.\n");
                    if(origin_idx != -1) { new_balance = client_table[origin_idx].balance; }
                }
                //envia resposta ack
                packet ack_pkt;
                ack_pkt.type = TYPE_ACK_REQ;
                ack_pkt.balance = new_balance;
                ack_pkt.seqn = pkt.seqn; //ack do número de sequencia recebido
                sendto(sockfd, &ack_pkt, sizeof(packet), 0, (const struct sockaddr *)&client_addr, len);

                get_current_time(time_str, sizeof(time_str));
                printf("%s client %s id_req %u dest %s value %u num_transactions %u total_transferred %u total_balance %u\n",
                    time_str,
                    inet_ntoa(client_addr.sin_addr), // IP Origem 
                    pkt.seqn, 
                    inet_ntoa(pkt.dest_addr), // IP Destino 
                    pkt.value, 
                    num_transactions, 
                    total_transferred,  
                    total_balance);
            }
        }
    }

    close(sockfd);
    return 0;

}