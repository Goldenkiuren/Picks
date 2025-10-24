#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <pthread.h>
#include <stdbool.h>
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
pthread_mutex_t global_mutex;

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



//estrutura para passar dados para a thread
typedef struct {
    packet pkt;
    struct sockaddr_in client_addr;
    socklen_t len;
    int sockfd;
} request_data;


//função executada por nova thread
void* process_request(void* arg) {

    //recupera os dados da requisicao
    request_data* data = (request_data*)arg;
    packet pkt = data->pkt;
    struct sockaddr_in client_addr = data->client_addr;
    int sockfd = data->sockfd;
    socklen_t len = data->len;
    
    //lógica de processamento 
    pthread_mutex_lock(&global_mutex);
    
    //lógica de descoberta
    if (ntohs(pkt.type) == TYPE_DESCOBERTA) {
        printf("Pedido de descoberta recebido de %s\n", inet_ntoa(client_addr.sin_addr));

        int client_idx = find_client(&client_addr);
        if (client_idx == -1) {
            register_new_client(&client_addr);
        }

        packet ack_pkt;
        memset(&ack_pkt, 0, sizeof(packet));
        ack_pkt.type = htons(TYPE_ACK_DESCOBERTA);
        sendto(sockfd, &ack_pkt, sizeof(packet), 0, (const struct sockaddr *)&client_addr, len);                
    }
    
    //lógica de requisição
    else if (ntohs(pkt.type) == TYPE_REQ) {
        uint32_t seqn = ntohl(pkt.seqn);
        uint32_t value = ntohl(pkt.value);

        int origin_idx = find_client(&client_addr);
        int dest_idx = find_client_ip(pkt.dest_addr);
        
        uint32_t new_balance = 0;
        
        bool send_ack = true; //controlar envio de ack e error
        
        if (origin_idx == -1) {
            printf("Cliente de origem não encontrado: %s\n", inet_ntoa(client_addr.sin_addr));
            packet error_pkt;
            memset(&error_pkt, 0, sizeof(packet));
            error_pkt.type = htons(TYPE_ERROR_REQ);
            sendto(sockfd, &error_pkt, sizeof(packet), 0, (const struct sockaddr *)&client_addr, len);
            
        } else if (dest_idx == -1) {
            printf("Cliente de destino não encontrado: %s\n", inet_ntoa(pkt.dest_addr));
            packet error_pkt;
            memset(&error_pkt, 0, sizeof(packet));
            error_pkt.type = htons(TYPE_ERROR_REQ);
            sendto(sockfd, &error_pkt, sizeof(packet), 0, (const struct sockaddr *)&client_addr, len);
        } else {
            uint32_t expected_seqn = client_table[origin_idx].last_req + 1;
            uint32_t current_balance = (uint32_t)client_table[origin_idx].balance;

            //Pacote novo e esperado
            if (seqn == expected_seqn) {
                
                uint32_t new_balance = current_balance;

                if (origin_idx == dest_idx) {
                    printf("Erro: Cliente %s tentando transferir para si mesmo.\n", inet_ntoa(client_addr.sin_addr));
                    // (Envia ACK de falha abaixo)
                }
                // Verifica saldo
                else if (client_table[origin_idx].balance >= (int32_t)value) {
                    client_table[origin_idx].balance -= (int32_t)value;
                    client_table[dest_idx].balance += (int32_t)value;
                    client_table[origin_idx].last_req = seqn;
                    new_balance = (uint32_t)client_table[origin_idx].balance;

                    num_transactions++;
                    total_transferred += value;
                    
                    // (Log da transação)
                    char time_str[100];
                    char ip_origin[INET_ADDRSTRLEN];
                    char ip_dest[INET_ADDRSTRLEN];
                    strcpy(ip_origin, inet_ntoa(client_addr.sin_addr));
                    strcpy(ip_dest, inet_ntoa(pkt.dest_addr));
                    get_current_time(time_str, sizeof(time_str));
                    printf("%s client %s id_req %u dest %s value %u num_transactions %u total_transferred %u total_balance %u\n",
                           time_str, ip_origin, seqn, ip_dest, value, 
                           num_transactions, total_transferred, total_balance);
                
                } else {
                    printf("Saldo insuficiente para cliente %s (saldo: %d, value: %u)\n", 
                           inet_ntoa(client_addr.sin_addr), client_table[origin_idx].balance, value);
                }

                // Envia ACK para a requisição processada (ou falha por saldo/auto-transf)
                packet ack_pkt;
                memset(&ack_pkt, 0, sizeof(packet));
                ack_pkt.type = htons(TYPE_ACK_REQ);
                ack_pkt.balance = htonl(new_balance);
                ack_pkt.seqn = htonl(seqn);
                sendto(sockfd, &ack_pkt, sizeof(packet), 0, (const struct sockaddr *)&client_addr, len);

            }
            //Pacote duplicado (seqn <= last_req)
            //Pacote fora de ordem (seqn > expected_seqn)
            else {
                if (seqn <= client_table[origin_idx].last_req) {
                    // Formato de log de duplicata conforme especificação [cite: 131]
                    char time_str[100];
                    char ip_origin[INET_ADDRSTRLEN];
                    char ip_dest[INET_ADDRSTRLEN];
                    
                    get_current_time(time_str, sizeof(time_str));
                    strcpy(ip_origin, inet_ntoa(client_addr.sin_addr));
                    strcpy(ip_dest, inet_ntoa(pkt.dest_addr));
                    
                    // Imprime o log formatado com "DUP!!"
                    printf("%s client %s DUP!! id req %u dest %s value %u num_transactions %u total_transferred %u total_balance %u\n",
                           time_str, ip_origin, seqn, ip_dest, value, 
                           num_transactions, total_transferred, total_balance);
                } else {
                    printf("Requisição fora de ordem recebida de %s (seqn: %u, esperado: %u). Enviando ACK do último.\n", 
                           inet_ntoa(client_addr.sin_addr), seqn, expected_seqn);
                }
                
                // Reenviar o ACK da *última* requisição processada
                packet ack_pkt;
                memset(&ack_pkt, 0, sizeof(packet));
                ack_pkt.type = htons(TYPE_ACK_REQ);
                ack_pkt.balance = htonl(current_balance);
                ack_pkt.seqn = htonl(client_table[origin_idx].last_req);
                sendto(sockfd, &ack_pkt, sizeof(packet), 0, (const struct sockaddr *)&client_addr, len);
            }
        }
    }
    
    //tratamento para outros types
    else if(ntohs(pkt.type) == TYPE_ERROR_REQ) {
        printf("Pacote de erro recebido de %s (ignorado)\n", inet_ntoa(client_addr.sin_addr));
    }
    else {
        printf("Tipo de pacote desconhecido (%d) de %s\n", ntohs(pkt.type), inet_ntoa(client_addr.sin_addr));
    }
    
    pthread_mutex_unlock(&global_mutex);
    
    free(arg);
    return NULL;
            
            
}



int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Uso: ./servidor <porta>\n");
        return 1;
    }

    int port = atoi(argv[1]);
    int sockfd;
    struct sockaddr_in server_addr;
    
    if (pthread_mutex_init(&global_mutex, NULL) != 0) {
        perror("falha ao inicializar mutex.\n");
        exit(EXIT_FAILURE);
    }

    // setup
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("falha em criar o socket");
        exit(EXIT_FAILURE);
    }
    
    
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);
    if (bind(sockfd, (const struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("falha no bind");
        exit(EXIT_FAILURE);
    }
    
    //log inicial
    char time_str[100];
    get_current_time(time_str, sizeof(time_str));
    printf("%s num_transactions %u total_transferred %u total_balance %u\n", time_str, num_transactions, total_transferred, total_balance);
    printf("Servidor UDP escutando na porta %d\n", port);


    while(1) {
        struct sockaddr_in client_addr_temp;
        packet pkt_temp;
        
        socklen_t len = sizeof(client_addr_temp);
        int n = recvfrom(sockfd, &pkt_temp, sizeof(packet), 0, (struct sockaddr *)&client_addr_temp, &len);
        
        if (n>0) {
            //alocar memoria no heap para os dados da requisicao
            request_data* data = (request_data*)malloc(sizeof(request_data));
            if (data == NULL) {
                perror("falha ao alocar memória para thread.\n");
                continue;
            }
            
            //copiar dados para a struct alocada
            data->pkt = pkt_temp;
            data->client_addr = client_addr_temp;
            data->len = len;
            data->sockfd = sockfd;
            
            //thread para processar requisicao
            pthread_t thread_id;
            if (pthread_create(&thread_id, NULL, process_request, (void*)data) != 0) {
                perror("falha ao criar thread");
                free(data); // Libera memória se a thread não foi criada
            }
            
            pthread_detach(thread_id);
        }
    }

    close(sockfd);
    pthread_mutex_destroy(&global_mutex);
    return 0;

}
