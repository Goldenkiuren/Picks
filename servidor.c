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

//constantes globais
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

pthread_mutex_t client_table_mutex; //mutex para adicoes e buscas na tabela
pthread_mutex_t stats_mutex;        //mutex para acessar estatisticas globais


//nó de uma lista para a fila de logs
typedef struct log_node {
    char text[LOG_MSG_LEN];
    struct log_node *next;
} log_node_t;

//variaveis para sistema de log
static log_node_t *log_head = NULL; 
static log_node_t *log_tail = NULL;
static pthread_mutex_t log_mutex;   //mutex para acessar fila de logs
static pthread_cond_t  update_cond; //variavel de condicao para sinalizar para a thread de -
                                    //- interface que novos logs estao disponiveis

/* 
aloca um nó. copia a mensagem de log para ele. adiciona ao final da fila. sinaliza para a interface o novo item.
*/
static void push_log(const char *txt) {
    log_node_t *n = malloc(sizeof(log_node_t));

    if (!n) {return;} //falha na alocação

    strncpy(n->text, txt, LOG_MSG_LEN-1);
    n->text[LOG_MSG_LEN-1] = '\0';
    n->next = NULL;

    pthread_mutex_lock(&log_mutex);
    if (log_tail) {
        log_tail->next = n;     //adicionado ao final
    }

    else {
        log_head = n;           //fila vazia
    }

    log_tail = n;

    //sinaliza a atualização
    pthread_cond_signal(&update_cond);
    pthread_mutex_unlock(&log_mutex);
}

/*
thread para imprimir logs.
fica a maior parte do tempo bloqueada, aguardando a variável de condição 'update_cond'.
quando ativada, imprime todos os logs na fila e volta a aguardar.
*/
static void *interface_thread(void *arg) {
    (void)arg;                      //evitar "unused parameter"
    pthread_mutex_lock(&log_mutex); //trava inicial
    while (1) {
        while (log_head == NULL) {
            //espera por novas atualizações
            pthread_cond_wait(&update_cond, &log_mutex);
        }

        //imprime todos os logs atualmente na fila 
        while (log_head) {
            log_node_t *n = log_head;
            log_head = n->next;
            if (log_head == NULL) log_tail = NULL;
            
            printf("%s\n", n->text);
            fflush(stdout);
            free(n);
        }
    }
    pthread_mutex_unlock(&log_mutex);
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

/*
registra um novo cliente
adiciona o cliente em 'client_table'. inicializa seu saldo. sera seu 'seqn'. inicializa seu mutex. 
atualiza estatisticas globais.
*/
int register_new_client(struct sockaddr_in* cliaddr) {
    if (num_clients < MAX_CLIENTS) {
        int new_client_id = num_clients;
        client_table[new_client_id].client_ip = cliaddr->sin_addr;
        client_table[new_client_id].last_req = 0;
        client_table[new_client_id].balance = INITIAL_BALANCE;

        //mutex especifico do cliente
        if (pthread_mutex_init(&client_table[new_client_id].client_lock, NULL) != 0) {
            return -1; 
        }

        num_clients++;

        //atualiza estatisticas globais
        uint32_t current_total_balance;
        pthread_mutex_lock(&stats_mutex);
        total_balance += INITIAL_BALANCE;
        current_total_balance = total_balance;
        uint32_t local_num_trans = num_transactions;
        uint32_t local_total_trans = total_transferred;
        pthread_mutex_unlock(&stats_mutex);
        
        //loga o registro do novo cliente
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

/*
função executada por nova thread.
ela executa em uma nova thread para cada pacote recebido. lida com a descoberta de clientes.
lida também com a descoberta de clientes e com as requisicoes de transação/consulta.
*/
void* process_request(void* arg) {

    //recupera os dados da requisicao
    request_data* data = (request_data*)arg;
    packet pkt = data->pkt;
    struct sockaddr_in client_addr = data->client_addr;
    int sockfd = data->sockfd;
    socklen_t len = data->len;
    
    //buffers para logs
    char logbuf[LOG_MSG_LEN];
    char time_str[100];
    char ip_origin[INET_ADDRSTRLEN];
    char ip_dest[INET_ADDRSTRLEN];

    //lógica de descoberta
    if (ntohs(pkt.type) == TYPE_DESCOBERTA) {
        pthread_mutex_lock(&client_table_mutex);        //trava tabela de clientes para verificar e registrar
        int client_idx = find_client(&client_addr);
        
        if (client_idx == -1) {
            register_new_client(&client_addr);  //registro de cliente novo
        }

        //responde com ACK de descoberta
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

        //busca IDs dos clientes de origem e destino
        pthread_mutex_lock(&client_table_mutex);
        int origin_idx = find_client(&client_addr);
        int dest_idx = find_client_ip(pkt.dest_addr);
        pthread_mutex_unlock(&client_table_mutex);

        uint32_t new_balance = 0;
        
        bool send_ack = true; //controlar envio de ack e error
        
        if (origin_idx == -1) { //cliente de origem ou destino desconhecido
            packet error_pkt;
            memset(&error_pkt, 0, sizeof(packet));
            error_pkt.type = htons(TYPE_ERROR_REQ);
            sendto(sockfd, &error_pkt, sizeof(packet), 0, (const struct sockaddr *)&client_addr, len);
            
        }
        
        else if (dest_idx == -1) {    //cliente de destino não existe
            packet error_pkt;
            memset(&error_pkt, 0, sizeof(packet));
            error_pkt.type = htons(TYPE_ERROR_REQ);
            sendto(sockfd, &error_pkt, sizeof(packet), 0, (const struct sockaddr *)&client_addr, len);
        } 
        
        else {
            //lógica de travamento
            bool self_transfer = (origin_idx == dest_idx);
            int lock1_idx = origin_idx;
            int lock2_idx = dest_idx;
            
            //garante que o mutex com indice menor seja travado primeiro (evita deadlock)
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
                    // 1. a consulta de saldo é uma requisição válida, então logamos
                    //(não altera num_transactions ou total_transferred)
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
                    
                    // 2. envia ACK com saldo ATUAL e seqn ATUAL
                    packet ack_pkt;
                    memset(&ack_pkt, 0, sizeof(packet));
                    ack_pkt.type = htons(TYPE_ACK_REQ);
                    ack_pkt.balance = htonl(current_balance); 
                    ack_pkt.seqn = htonl(seqn);
                    sendto(sockfd, &ack_pkt, sizeof(packet), 0, (const struct sockaddr *)&client_addr, len);
                    
                    // 3. atualiza o last_req 
                    // sem isso o, o cliente vai ficar reenviando a consulta.
                    client_table[origin_idx].last_req = seqn;
                    
                    // 4. libera travas e encerra a thread
                    pthread_mutex_unlock(&client_table[lock1_idx].client_lock);
                    if (!self_transfer) {
                        pthread_mutex_unlock(&client_table[lock2_idx].client_lock);
                    }
                    free(arg); 
                    return NULL; //termina a thread
                }
                
                if (self_transfer) {} //auto-transferencia nao faz nada
                
                //verifica se tem saldo suficiente
                else if (current_balance >= value) {
                    //executa a transferencia
                    client_table[origin_idx].balance -= (int32_t)value;
                    client_table[dest_idx].balance += (int32_t)value;
                    new_balance = (uint32_t)client_table[origin_idx].balance;

                    //atualiza estatisticas globais (transferencia bem-sucedida)
                    uint32_t local_num_trans, local_total_trans, local_total_bal;
                    pthread_mutex_lock(&stats_mutex);
                    num_transactions++;
                    total_transferred += value;
                    local_num_trans = num_transactions;
                    local_total_trans = total_transferred;
                    local_total_bal = total_balance;
                    pthread_mutex_unlock(&stats_mutex);                 
                }
                else {} //saldo insuficiente. 'new_balance' continua 'current_balance'
                
                //atualiza o ultimo seqn processado para este cliente
                client_table[origin_idx].last_req = seqn;
                last_processed_seqn = seqn;

                //pega estatisticas para o log (mesmo se a transacao falhou por saldo)
                uint32_t local_num_trans, local_total_trans, local_total_bal;
                pthread_mutex_lock(&stats_mutex);
                local_num_trans = num_transactions;
                local_total_trans = total_transferred;
                local_total_bal = total_balance;
                pthread_mutex_unlock(&stats_mutex);
                
                //loga a tentativa de transferencia
                get_current_time(time_str, sizeof(time_str));
                strcpy(ip_origin, inet_ntoa(client_addr.sin_addr));
                strcpy(ip_dest, inet_ntoa(pkt.dest_addr));
                
                snprintf(logbuf, sizeof(logbuf),
                         "%s client %s id req %u dest %s value %u num_transactions %u total_transferred %u total_balance %u",
                         time_str, ip_origin, seqn, ip_dest, value, 
                         local_num_trans, local_total_trans, local_total_bal);
                push_log(logbuf);

                //envia ACK para a requisição processada (com sucesso ou falha)
                packet ack_pkt;
                memset(&ack_pkt, 0, sizeof(packet));
                ack_pkt.type = htons(TYPE_ACK_REQ);
                ack_pkt.balance = htonl(new_balance);   // o novo saldo (ou o antigo se falhou)
                ack_pkt.seqn = htonl(seqn);             // confirma o seqn da requisicao
                sendto(sockfd, &ack_pkt, sizeof(packet), 0, (const struct sockaddr *)&client_addr, len);
            }

            //pacote duplicado (seqn <= last_req) ou pacote fora de ordem (seqn > expected_seqn)
            else {

                //se for duplicata, loga como "DUP!!""
                if (seqn <= client_table[origin_idx].last_req) {
                    
                    //log de duplicata 
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
                } 
                
                //se for fora de orgem (pacote do futuro), loga normalmente
                else {
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
                
                //reenviar o ACK da ultima requisicao processada
                packet ack_pkt;
                memset(&ack_pkt, 0, sizeof(packet));
                ack_pkt.type = htons(TYPE_ACK_REQ);
                ack_pkt.balance = htonl(current_balance);                   //saldo atual (resultado do ultimo ACK)
                ack_pkt.seqn = htonl(client_table[origin_idx].last_req);    //seqn do ultimo ACK
                sendto(sockfd, &ack_pkt, sizeof(packet), 0, (const struct sockaddr *)&client_addr, len);
            }

            //fim da secao critica
            //libera as travas na ordem inversa da aquisicao
            pthread_mutex_unlock(&client_table[lock1_idx].client_lock);
            if (!self_transfer) {
                pthread_mutex_unlock(&client_table[lock2_idx].client_lock);
            }
        }
    }
    
    //tratamento para outros types
    else if(ntohs(pkt.type) == TYPE_ERROR_REQ) {} //ignora erros
    else {}  //ignora tipos de pacotes desconhecidos
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
    
    //inicializa os mutexes e variaveis de condicao globais
    if (pthread_mutex_init(&client_table_mutex, NULL) != 0 || 
            pthread_mutex_init(&stats_mutex, NULL) != 0 ||
            pthread_mutex_init(&log_mutex, NULL) != 0 ||
            pthread_cond_init(&update_cond, NULL) != 0) {
            perror("falha ao inicializar mutexes/cond globais.\n");
            exit(EXIT_FAILURE);
    }

    // configura o socket udp
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("falha em criar o socket");
        exit(EXIT_FAILURE);
    }
    
    //zera a estrutura de endereço do servidor
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;   //escuta em todas as interfaces de rede
    server_addr.sin_port = htons(port);         //converte a porta pra "network byte order"


    //vincula o scoket a porta e endereco especificados
    if (bind(sockfd, (const struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("falha no bind");
        exit(EXIT_FAILURE);
    }
    
    //inicialização da thread de interface/log
    pthread_t int_tid;
    if (pthread_create(&int_tid, NULL, interface_thread, NULL) != 0) {
        perror("falha ao criar thread de interface");
        close(sockfd);
        exit(EXIT_FAILURE);
    }
    pthread_detach(int_tid);    //nao há join nela, ela roda sempre

    //log inicial
    char time_str[100];
    get_current_time(time_str, sizeof(time_str));
    pthread_mutex_lock(&stats_mutex);

    //imprime o log inicial diretamente, pois a thread de log já pode estar rodando
    printf("%s num_transactions %u total_transferred %u total_balance %u\n", 
        time_str, num_transactions, total_transferred, total_balance);
    pthread_mutex_unlock(&stats_mutex);
    
    
    while(1) {
        struct sockaddr_in client_addr_temp;    //endereço do cliente(temporario)
        packet pkt_temp;                        //pacote recebido (temporario)
        
        socklen_t len = sizeof(client_addr_temp);
        
        //aguarda a chegada de um pacote UDP
        int n = recvfrom(sockfd, &pkt_temp, sizeof(packet), 0, (struct sockaddr *)&client_addr_temp, &len);
        
        if (n>0) {  //pacote recebido


            // aloca memoria no heap para os dados da requisicao
            request_data* data = (request_data*)malloc(sizeof(request_data));
            
            if (data == NULL) {
                perror("falha ao alocar memória para thread.\n");
                continue;
            }
            
            //copia dados do pacote e do cliente para a struct alocada no heap
            data->pkt = pkt_temp;
            data->client_addr = client_addr_temp;
            data->len = len;
            data->sockfd = sockfd;      //passa o socket para a thread poder responder
            
            //thread para processar requisicao
            pthread_t thread_id;
            if (pthread_create(&thread_id, NULL, process_request, (void*)data) != 0) {
                perror("falha ao criar thread");
                free(data); //libera memória se a thread não foi criada
            }
            
             
            pthread_detach(thread_id);
        }
    }

    close(sockfd);
    pthread_mutex_destroy(&client_table_mutex);
    pthread_mutex_destroy(&stats_mutex);
    pthread_mutex_destroy(&log_mutex);
    pthread_cond_destroy(&update_cond);

    //destroi mutexes individuais de cada cliente
    for (int i = 0; i < num_clients; i++) {
        pthread_mutex_destroy(&client_table[i].client_lock);
    }
    
    return 0;

}