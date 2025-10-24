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
#define LOG_MSG_LEN 256

//globais do servidor
client_data client_table[MAX_CLIENTS];
int num_clients = 0;
uint32_t num_transactions = 0;
uint32_t total_transferred = 0;
uint32_t total_balance = 0;
pthread_mutex_t client_table_mutex;
pthread_mutex_t stats_mutex;

typedef struct log_node {
    char text[LOG_MSG_LEN];
    struct log_node *next;
} log_node_t;

static log_node_t *log_head = NULL;
static log_node_t *log_tail = NULL;
static pthread_mutex_t log_mutex;
static pthread_cond_t  update_cond;

/* Adiciona log na fila e sinaliza thread de interface */
static void push_log(const char *txt) {
    log_node_t *n = malloc(sizeof(log_node_t));
    if (!n) return;
    strncpy(n->text, txt, LOG_MSG_LEN-1);
    n->text[LOG_MSG_LEN-1] = '\0';
    n->next = NULL;

    pthread_mutex_lock(&log_mutex);
    if (log_tail) log_tail->next = n;
    else log_head = n;
    log_tail = n;
    /* Sinaliza que uma nova atualização está disponível */
    pthread_cond_signal(&update_cond);
    pthread_mutex_unlock(&log_mutex);
}

/* Thread de interface: aguarda por logs e os imprime (bloqueia até atualização) */
static void *interface_thread(void *arg) {
    (void)arg;
    pthread_mutex_lock(&log_mutex); // Trava inicial
    while (1) {
        while (log_head == NULL) {
            /* espera por novas atualizações */
            pthread_cond_wait(&update_cond, &log_mutex);
        }
        /* pop e imprime todos os logs atualmente na fila */
        while (log_head) {
            log_node_t *n = log_head;
            log_head = n->next;
            if (log_head == NULL) log_tail = NULL;
            
            printf("%s\n", n->text);
            fflush(stdout);
            free(n);
        }
    }
    pthread_mutex_unlock(&log_mutex); // Nunca alcançado, mas bom costume
    return NULL;
}

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
        if (pthread_mutex_init(&client_table[new_client_id].client_lock, NULL) != 0) {
            return -1; 
        }
        num_clients++;
        uint32_t current_total_balance;
        pthread_mutex_lock(&stats_mutex);
        total_balance += INITIAL_BALANCE;
        current_total_balance = total_balance;
        uint32_t local_num_trans = num_transactions;
        uint32_t local_total_trans = total_transferred;
        pthread_mutex_unlock(&stats_mutex);
        char time_str[100];
        char logbuf[LOG_MSG_LEN];
        get_current_time(time_str, sizeof(time_str));
        snprintf(logbuf, sizeof(logbuf),
                 "%s client %s id req 0 dest 0 value 0 num_transactions %u total_transferred %u total_balance %u",
                 time_str,
                 inet_ntoa(cliaddr->sin_addr),
                 local_num_trans,
                 local_total_trans,
                 current_total_balance);
        push_log(logbuf);        
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
    
    char logbuf[LOG_MSG_LEN];
    char time_str[100];
    char ip_origin[INET_ADDRSTRLEN];
    char ip_dest[INET_ADDRSTRLEN];

    //lógica de descoberta
    if (ntohs(pkt.type) == TYPE_DESCOBERTA) {
        pthread_mutex_lock(&client_table_mutex);
        int client_idx = find_client(&client_addr);
        if (client_idx == -1) {
            register_new_client(&client_addr);
        }
        pthread_mutex_unlock(&client_table_mutex);
        packet ack_pkt;
        memset(&ack_pkt, 0, sizeof(packet));
        ack_pkt.type = htons(TYPE_ACK_DESCOBERTA);
        sendto(sockfd, &ack_pkt, sizeof(packet), 0, (const struct sockaddr *)&client_addr, len);                
    }
    
    //lógica de requisição
    else if (ntohs(pkt.type) == TYPE_REQ) {
        uint32_t seqn = ntohl(pkt.seqn);
        uint32_t value = ntohl(pkt.value);
        pthread_mutex_lock(&client_table_mutex);
        int origin_idx = find_client(&client_addr);
        int dest_idx = find_client_ip(pkt.dest_addr);
        pthread_mutex_unlock(&client_table_mutex);

        uint32_t new_balance = 0;
        
        bool send_ack = true; //controlar envio de ack e error
        
        if (origin_idx == -1) {
            packet error_pkt;
            memset(&error_pkt, 0, sizeof(packet));
            error_pkt.type = htons(TYPE_ERROR_REQ);
            sendto(sockfd, &error_pkt, sizeof(packet), 0, (const struct sockaddr *)&client_addr, len);
            
        } else if (dest_idx == -1) {
            packet error_pkt;
            memset(&error_pkt, 0, sizeof(packet));
            error_pkt.type = htons(TYPE_ERROR_REQ);
            sendto(sockfd, &error_pkt, sizeof(packet), 0, (const struct sockaddr *)&client_addr, len);
        } else {
            bool self_transfer = (origin_idx == dest_idx);
            int lock1_idx = origin_idx;
            int lock2_idx = dest_idx;

            if (!self_transfer) {
                lock1_idx = (origin_idx < dest_idx) ? origin_idx : dest_idx;
                lock2_idx = (origin_idx > dest_idx) ? origin_idx : dest_idx;
            }

            //bloqueia mutex clientes
            pthread_mutex_lock(&client_table[lock1_idx].client_lock);
            if (!self_transfer) {
                pthread_mutex_lock(&client_table[lock2_idx].client_lock);
            }

            //seção critica clientes
            uint32_t expected_seqn = client_table[origin_idx].last_req + 1;
            uint32_t current_balance = (uint32_t)client_table[origin_idx].balance;
            new_balance = current_balance;
            uint32_t last_processed_seqn = client_table[origin_idx].last_req;

            //Pacote novo e esperado
            if (seqn == expected_seqn) {
                if (value == 0) {
                    // 1. A consulta de saldo é uma requisição válida, então logamos.
                    // Não altera num_transactions ou total_transferred.
                    uint32_t local_num_trans, local_total_trans, local_total_bal;
                    pthread_mutex_lock(&stats_mutex);
                    local_num_trans = num_transactions;
                    local_total_trans = total_transferred;
                    local_total_bal = total_balance;
                    pthread_mutex_unlock(&stats_mutex);

                    get_current_time(time_str, sizeof(time_str));
                    strcpy(ip_origin, inet_ntoa(client_addr.sin_addr));
                    strcpy(ip_dest, inet_ntoa(pkt.dest_addr));
                    
                    snprintf(logbuf, sizeof(logbuf),
                             "%s client %s id req %u dest %s value 0 num_transactions %u total_transferred %u total_balance %u",
                             time_str, ip_origin, seqn, ip_dest,
                             local_num_trans, local_total_trans, local_total_bal);
                    push_log(logbuf);
                    
                    // 2. Envia ACK com saldo ATUAL e seqn ATUAL
                    packet ack_pkt;
                    memset(&ack_pkt, 0, sizeof(packet));
                    ack_pkt.type = htons(TYPE_ACK_REQ);
                    ack_pkt.balance = htonl(current_balance); 
                    ack_pkt.seqn = htonl(seqn);
                    sendto(sockfd, &ack_pkt, sizeof(packet), 0, (const struct sockaddr *)&client_addr, len);
                    
                    // 3. Atualiza o last_req (MUITO IMPORTANTE)
                    // Se não fizermos isso, o cliente ficará reenviando a consulta.
                    client_table[origin_idx].last_req = seqn;
                    
                    // 4. Libera travas e encerra a thread
                    pthread_mutex_unlock(&client_table[lock1_idx].client_lock);
                    if (!self_transfer) {
                        pthread_mutex_unlock(&client_table[lock2_idx].client_lock);
                    }
                    free(arg); 
                    return NULL; // Termina a thread
                }
                if (self_transfer) {}
                // Verifica saldo
                else if (current_balance >= value) {
                    client_table[origin_idx].balance -= (int32_t)value;
                    client_table[dest_idx].balance += (int32_t)value;
                    new_balance = (uint32_t)client_table[origin_idx].balance;
                    uint32_t local_num_trans, local_total_trans, local_total_bal;
                    pthread_mutex_lock(&stats_mutex);
                    num_transactions++;
                    total_transferred += value;
                    local_num_trans = num_transactions;
                    local_total_trans = total_transferred;
                    local_total_bal = total_balance;
                    pthread_mutex_unlock(&stats_mutex);                 
                } else {}
                client_table[origin_idx].last_req = seqn;
                last_processed_seqn = seqn;
                uint32_t local_num_trans, local_total_trans, local_total_bal;
                pthread_mutex_lock(&stats_mutex);
                local_num_trans = num_transactions;
                local_total_trans = total_transferred;
                local_total_bal = total_balance;
                pthread_mutex_unlock(&stats_mutex);
                
                get_current_time(time_str, sizeof(time_str));
                strcpy(ip_origin, inet_ntoa(client_addr.sin_addr));
                strcpy(ip_dest, inet_ntoa(pkt.dest_addr));
                
                snprintf(logbuf, sizeof(logbuf),
                         "%s client %s id req %u dest %s value %u num_transactions %u total_transferred %u total_balance %u",
                         time_str, ip_origin, seqn, ip_dest, value, 
                         local_num_trans, local_total_trans, local_total_bal);
                push_log(logbuf);

                // Envia ACK para a requisição processada (ou falha por saldo/auto-transf)
                packet ack_pkt;
                memset(&ack_pkt, 0, sizeof(packet));
                ack_pkt.type = htons(TYPE_ACK_REQ);
                ack_pkt.balance = htonl(new_balance);
                ack_pkt.seqn = htonl(seqn);
                sendto(sockfd, &ack_pkt, sizeof(packet), 0, (const struct sockaddr *)&client_addr, len);
            }
            //Pacote duplicado (seqn <= last_req) ou Pacote fora de ordem (seqn > expected_seqn)
            else {
                if (seqn <= client_table[origin_idx].last_req) {
                    // Formato de log de duplicata conforme especificação
                    char time_str[100];
                    char ip_origin[INET_ADDRSTRLEN];
                    char ip_dest[INET_ADDRSTRLEN];
                    
                    get_current_time(time_str, sizeof(time_str));
                    strcpy(ip_origin, inet_ntoa(client_addr.sin_addr));
                    strcpy(ip_dest, inet_ntoa(pkt.dest_addr));
                    uint32_t local_num_trans, local_total_trans, local_total_bal;
                    pthread_mutex_lock(&stats_mutex);
                    local_num_trans = num_transactions;
                    local_total_trans = total_transferred;
                    local_total_bal = total_balance;
                    pthread_mutex_unlock(&stats_mutex);
                    snprintf(logbuf, sizeof(logbuf),
                           "%s client %s DUP!! id req %u dest %s value %u num_transactions %u total_transferred %u total_balance %u",
                           time_str, ip_origin, seqn, ip_dest, value, 
                           local_num_trans, local_total_trans, local_total_bal);
                    push_log(logbuf);
                } else {
                    get_current_time(time_str, sizeof(time_str));
                    strcpy(ip_origin, inet_ntoa(client_addr.sin_addr));
                    strcpy(ip_dest, inet_ntoa(pkt.dest_addr));
                    uint32_t local_num_trans, local_total_trans, local_total_bal;
                    pthread_mutex_lock(&stats_mutex);
                    local_num_trans = num_transactions;
                    local_total_trans = total_transferred;
                    local_total_bal = total_balance;
                    pthread_mutex_unlock(&stats_mutex);
                    
                    snprintf(logbuf, sizeof(logbuf),
                             "%s client %s id req %u dest %s value %u num_transactions %u total_transferred %u total_balance %u",
                             time_str, ip_origin, seqn, ip_dest, value, 
                             local_num_trans, local_total_trans, local_total_bal);
                    push_log(logbuf);
                }
                
                // Reenviar o ACK da *última* requisição processada
                packet ack_pkt;
                memset(&ack_pkt, 0, sizeof(packet));
                ack_pkt.type = htons(TYPE_ACK_REQ);
                ack_pkt.balance = htonl(current_balance);
                ack_pkt.seqn = htonl(client_table[origin_idx].last_req);
                sendto(sockfd, &ack_pkt, sizeof(packet), 0, (const struct sockaddr *)&client_addr, len);
            }
            pthread_mutex_unlock(&client_table[lock1_idx].client_lock);
            if (!self_transfer) {
                pthread_mutex_unlock(&client_table[lock2_idx].client_lock);
            }
        }
    }
    
    //tratamento para outros types
    else if(ntohs(pkt.type) == TYPE_ERROR_REQ) {}
    else {}
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
    
    if (pthread_mutex_init(&client_table_mutex, NULL) != 0 || 
            pthread_mutex_init(&stats_mutex, NULL) != 0 ||
            pthread_mutex_init(&log_mutex, NULL) != 0 ||
            pthread_cond_init(&update_cond, NULL) != 0) {
            perror("falha ao inicializar mutexes/cond globais.\n");
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
    
    pthread_t int_tid;
    if (pthread_create(&int_tid, NULL, interface_thread, NULL) != 0) {
        perror("falha ao criar thread de interface");
        close(sockfd);
        exit(EXIT_FAILURE);
    }
    pthread_detach(int_tid);

    //log inicial
    char time_str[100];
    get_current_time(time_str, sizeof(time_str));
    pthread_mutex_lock(&stats_mutex);
    printf("%s num_transactions %u total_transferred %u total_balance %u\n", 
        time_str, num_transactions, total_transferred, total_balance);
    pthread_mutex_unlock(&stats_mutex);
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
    pthread_mutex_destroy(&client_table_mutex);
    pthread_mutex_destroy(&stats_mutex);
    pthread_mutex_destroy(&log_mutex);
    pthread_cond_destroy(&update_cond);
    for (int i = 0; i < num_clients; i++) {
        pthread_mutex_destroy(&client_table[i].client_lock);
    }
    return 0;

}