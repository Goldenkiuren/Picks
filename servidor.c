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
#include <errno.h>
#include "common.h"

// constantes globais
#define BUFFER_SIZE 1024
#define MAX_CLIENTS 100
#define INITIAL_BALANCE 100
#define LOG_MSG_LEN 256
#define MAX_REPLICAS 10 

// configurações de tempo para eleição
#define HEARTBEAT_INTERVAL_US 500000
#define ELECTION_TIMEOUT_US 1500000
#define WAIT_ANSWER_TIMEOUT_US 2500000

void get_current_time(char* buffer, size_t buffer_size);

// globais do servidor
client_data client_table[MAX_CLIENTS];
int num_clients = 0;
uint32_t num_transactions = 0;
uint32_t total_transferred = 0;
uint32_t total_balance = 0;
int sockfd;

// controle de replicação e eleição
int my_id = 0;
bool is_leader = false;
int current_leader_id = -1;
struct timespec last_heartbeat_time;

typedef struct {
    int id;
    struct sockaddr_in addr;
    bool active;
} replica_info;

replica_info replicas[MAX_REPLICAS];
int num_replicas = 0;

// mutexes
pthread_mutex_t client_table_mutex;
pthread_mutex_t stats_mutex;
pthread_mutex_t log_mutex;
pthread_cond_t  update_cond;

// mutex para controle de eleição
pthread_mutex_t election_mutex = PTHREAD_MUTEX_INITIALIZER;
bool election_in_progress = false;
bool received_answer = false; // flag para saber se alguém maior respondeu

// sistema de log
typedef struct log_node {
    char text[LOG_MSG_LEN];
    struct log_node *next;
} log_node_t;

static log_node_t *log_head = NULL; 
static log_node_t *log_tail = NULL;

static void push_log(const char *txt) {
    log_node_t *n = malloc(sizeof(log_node_t));
    if (!n) {return;} 
    strncpy(n->text, txt, LOG_MSG_LEN-1);
    n->text[LOG_MSG_LEN-1] = '\0';
    n->next = NULL;
    pthread_mutex_lock(&log_mutex);
    if (log_tail) { log_tail->next = n; }
    else { log_head = n; }
    log_tail = n;
    pthread_cond_signal(&update_cond);
    pthread_mutex_unlock(&log_mutex);
}

static void *interface_thread(void *arg) {
    (void)arg;
    pthread_mutex_lock(&log_mutex); 
    while (1) {
        while (log_head == NULL) {
            pthread_cond_wait(&update_cond, &log_mutex);
        }
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

// funções auxiliares
int find_client(struct sockaddr_in* cliaddr) {
    for (int i = 0; i < num_clients; i++) {
        if (client_table[i].client_ip.s_addr == cliaddr->sin_addr.s_addr) return i;
    }
    return -1;
}

int find_client_ip(struct in_addr ip_addr) {
    for (int i = 0; i < num_clients ; i++) {
        if (client_table[i].client_ip.s_addr == ip_addr.s_addr) return i;
    }
    return -1;
}

// helper para enviar pacote para uma réplica específica
void send_to_replica(int replica_idx, int type) {
    if (replicas[replica_idx].id == my_id) return;
    packet pkt;
    memset(&pkt, 0, sizeof(packet));
    pkt.type = htons(type);
    pkt.seqn = htonl(my_id);
    sendto(sockfd, &pkt, sizeof(packet), 0, 
          (struct sockaddr*)&replicas[replica_idx].addr, sizeof(struct sockaddr_in));
}

// broadcast para todas as réplicas
void broadcast_to_replicas(int type) {
    for(int i=0; i < num_replicas; i++) {
        send_to_replica(i, type);
    }
}

// replicação de transação
void replicate_transaction(struct in_addr src, struct in_addr dest, uint32_t value, uint32_t seqn, uint32_t balance_src) {
    packet rep_pkt;
    memset(&rep_pkt, 0, sizeof(packet));
    rep_pkt.type = htons(TYPE_REPLICATION);
    rep_pkt.src_addr = src;
    rep_pkt.dest_addr = dest;
    rep_pkt.value = htonl(value);
    rep_pkt.seqn = htonl(seqn);
    rep_pkt.balance = htonl(balance_src);
    
    for(int i=0; i < num_replicas; i++) {
        sendto(sockfd, &rep_pkt, sizeof(packet), 0, 
              (struct sockaddr*)&replicas[i].addr, sizeof(struct sockaddr_in));
    }
}

int register_new_client(struct sockaddr_in* cliaddr) {
    if (num_clients < MAX_CLIENTS) {
        int new_client_id = num_clients;
        client_table[new_client_id].client_ip = cliaddr->sin_addr;
        client_table[new_client_id].last_req = 0;
        client_table[new_client_id].balance = INITIAL_BALANCE;

        if (pthread_mutex_init(&client_table[new_client_id].client_lock, NULL) != 0) return -1; 
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
        snprintf(logbuf, sizeof(logbuf), "%s REGISTRO client %s total_bal %u",
                 time_str, inet_ntoa(cliaddr->sin_addr), current_total_balance);
        push_log(logbuf);        
        return new_client_id;
    }
    return -1;
}

void get_current_time(char* buffer, size_t buffer_size) {
    time_t now = time(0);
    struct tm *t = localtime(&now);
    strftime(buffer, buffer_size, "%Y-%m-%d %H:%M:%S", t);
}

// assumir liderança
void become_leader() {
    pthread_mutex_lock(&election_mutex);
    is_leader = true;
    current_leader_id = my_id;
    election_in_progress = false;
    pthread_mutex_unlock(&election_mutex);

    char logbuf[LOG_MSG_LEN];
    snprintf(logbuf, sizeof(logbuf), "--- EU SOU O NOVO LIDER (ID %d) ---", my_id);
    push_log(logbuf);

    // avisa liderança
    broadcast_to_replicas(TYPE_COORDINATOR);
}

// inicia o processo de eleição
void start_election() {
    pthread_mutex_lock(&election_mutex);
    election_in_progress = true;
    received_answer = false;
    pthread_mutex_unlock(&election_mutex);

    char logbuf[LOG_MSG_LEN];
    snprintf(logbuf, sizeof(logbuf), "--- INICIANDO ELEICAO (Meu ID: %d) ---", my_id);
    push_log(logbuf);

    bool sent_election = false;
    // 1. envia ELECTION para todos com ID maior
    for(int i=0; i<num_replicas; i++) {
        if (replicas[i].id > my_id) {
            send_to_replica(i, TYPE_ELECTION);
            sent_election = true;
        }
    }

    // 2. se não há ninguém com ID maior, vence imediatamente
    if (!sent_election) {
        become_leader();
        return;
    }

    // 3. espera por respostas
    usleep(WAIT_ANSWER_TIMEOUT_US); 

    pthread_mutex_lock(&election_mutex);
    if (!received_answer) {
        // ninguém maior respondeu, ganha
        pthread_mutex_unlock(&election_mutex);
        become_leader();
    } else {
        // alguém maior respondeu, volta a ser backup e espera o COORDINATOR
        election_in_progress = false;
        pthread_mutex_unlock(&election_mutex);
        snprintf(logbuf, sizeof(logbuf), "--- Recebi ANSWER, aguardando novo Lider... ---");
        push_log(logbuf);
    }
}

// thread dedicada ao monitoramento e heartbeat
void* monitor_thread(void* arg) {
    clock_gettime(CLOCK_MONOTONIC, &last_heartbeat_time);

    while(1) {
        if (is_leader) {
            // envio heartbeats periodicamente
            broadcast_to_replicas(TYPE_HEARTBEAT);
            usleep(HEARTBEAT_INTERVAL_US);
        } else {
            // verifica se o líder morreu
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            
            long diff_us = (now.tv_sec - last_heartbeat_time.tv_sec) * 1000000 + 
                           (now.tv_nsec - last_heartbeat_time.tv_nsec) / 1000;

            if (diff_us > ELECTION_TIMEOUT_US) {
                // líder morto.
                if (!election_in_progress) {
                    char logbuf[LOG_MSG_LEN];
                    snprintf(logbuf, sizeof(logbuf), "--- TIMEOUT DETECTADO DO LIDER %d ---", current_leader_id);
                    push_log(logbuf);
                    
                    // reseta timer para não spammar
                    clock_gettime(CLOCK_MONOTONIC, &last_heartbeat_time);
                    start_election();
                }
            }
            usleep(200000); // check a cada 200ms
        }

    }
    return NULL;
}

typedef struct {
    packet pkt;
    struct sockaddr_in client_addr;
    socklen_t len;
    int sockfd;
} request_data;

void* process_request(void* arg) {
    request_data* data = (request_data*)arg;
    packet pkt = data->pkt;
    struct sockaddr_in client_addr = data->client_addr;
    int sockfd_local = data->sockfd;
    socklen_t len = data->len;
    
    char logbuf[LOG_MSG_LEN];
    char time_str[100];

    uint16_t type = ntohs(pkt.type);
    uint32_t sender_id = ntohl(pkt.seqn); // nas msgs de controle, seqn carrega o ID

    // --- TRATAMENTO DE MENSAGENS DE CONTROLE  ---

    if (type == TYPE_HEARTBEAT) {
        // atualiza timestamp
        if (!is_leader) {
            clock_gettime(CLOCK_MONOTONIC, &last_heartbeat_time);
            if (current_leader_id != (int)sender_id) {
                current_leader_id = (int)sender_id;
            }
        }
    }

    else if (type == TYPE_ELECTION) {
        // eleicao        
        packet ans_pkt;
        memset(&ans_pkt, 0, sizeof(packet));
        ans_pkt.type = htons(TYPE_ANSWER);
        ans_pkt.seqn = htonl(my_id);
        sendto(sockfd_local, &ans_pkt, sizeof(packet), 0, (struct sockaddr *)&client_addr, len);

        snprintf(logbuf, sizeof(logbuf), "Recebi ELECTION de %d. Enviei ANSWER.", sender_id);
        push_log(logbuf);

        if (!election_in_progress && !is_leader) {
            //  verificação rápida com lock antes de criar a thread
            pthread_mutex_lock(&election_mutex);
            bool should_start = !election_in_progress && !is_leader;
            if(should_start) election_in_progress = true;
            pthread_mutex_unlock(&election_mutex);

            if (should_start) {
                pthread_t elect_tid;
                pthread_create(&elect_tid, NULL, (void*)start_election, NULL);
                pthread_detach(elect_tid);
            }
        }
    }

    else if (type == TYPE_ANSWER) {
        // recebe resposta de alguém maior e desisto.
        pthread_mutex_lock(&election_mutex);
        if (election_in_progress) {
            received_answer = true;
            snprintf(logbuf, sizeof(logbuf), "Recebi ANSWER de %d. Respeitando autoridade.", sender_id);
            push_log(logbuf);
        }
        pthread_mutex_unlock(&election_mutex);
    }

    else if (type == TYPE_COORDINATOR) {
        // verifica se o novo líder é um impostor (menor que eu)
        if (sender_id < my_id) {
            char logbuf[LOG_MSG_LEN];
            snprintf(logbuf, sizeof(logbuf), "--- IGNORANDO COORDENADOR %d (SOU MAIOR: %d) E INICIANDO ELEICAO ---", sender_id, my_id);
            push_log(logbuf);
            
            if (!election_in_progress) {
                pthread_mutex_lock(&election_mutex);
                election_in_progress = true;
                pthread_mutex_unlock(&election_mutex);
                
                pthread_t elect_tid;
                pthread_create(&elect_tid, NULL, (void*)start_election, NULL);
                pthread_detach(elect_tid);
            }
            return NULL;
        }
        pthread_mutex_lock(&election_mutex);
        is_leader = false;
        current_leader_id = (int)sender_id;
        if (election_in_progress) {
            received_answer = true;
            push_log("Recebi COORDINATOR durante eleição. Abortando minha candidatura.");
        }
        pthread_mutex_unlock(&election_mutex);
    }
    // --- LÓGICA DE DESCOBERTA (CLIENTE) ---
    else if (type == TYPE_DESCOBERTA) {
        // apenas o LÍDER responde descoberta
        if (is_leader) {
            pthread_mutex_lock(&client_table_mutex);
            int client_idx = find_client(&client_addr);
            
            if (client_idx == -1) {
                int id = register_new_client(&client_addr);
                // replica criação do cliente
                if (id != -1) {
                    replicate_transaction(client_addr.sin_addr, client_addr.sin_addr, 0, 0, INITIAL_BALANCE);
                }
            }

            pthread_mutex_unlock(&client_table_mutex);
            packet ack_pkt;
            memset(&ack_pkt, 0, sizeof(packet));
            ack_pkt.type = htons(TYPE_ACK_DESCOBERTA);
            sendto(sockfd_local, &ack_pkt, sizeof(packet), 0, (const struct sockaddr *)&client_addr, len);
        }
    }

    // --- REPLICAÇÃO PASSIVA ---
    else if (type == TYPE_REPLICATION) {
        // backup recebe atualização de estado
        if (is_leader) { free(arg); return NULL; } // líder ignora replicação (loopback prevention)

        struct in_addr src_ip = pkt.src_addr;
        struct in_addr dest_ip = pkt.dest_addr;
        uint32_t val = ntohl(pkt.value);
        uint32_t r_seqn = ntohl(pkt.seqn);

        // reset heartbeat timer pois recebemos dados válidos do líder
        clock_gettime(CLOCK_MONOTONIC, &last_heartbeat_time);

        pthread_mutex_lock(&client_table_mutex);
        
        // sincroniza tabela de clientes
        int src_idx = find_client_ip(src_ip);
        if (src_idx == -1 && num_clients < MAX_CLIENTS) {
            src_idx = num_clients++;
            client_table[src_idx].client_ip = src_ip;
            client_table[src_idx].balance = INITIAL_BALANCE;
            pthread_mutex_init(&client_table[src_idx].client_lock, NULL);
        }

        int dest_idx = -1;
        if (val > 0 || dest_ip.s_addr != 0) {
             dest_idx = find_client_ip(dest_ip);
             if (dest_idx == -1 && num_clients < MAX_CLIENTS) {
                dest_idx = num_clients++;
                client_table[dest_idx].client_ip = dest_ip;
                client_table[dest_idx].balance = INITIAL_BALANCE;
                pthread_mutex_init(&client_table[dest_idx].client_lock, NULL);
            }
        }
        
        if (src_idx != -1) {
            pthread_mutex_lock(&client_table[src_idx].client_lock);
            if (r_seqn > client_table[src_idx].last_req) {
                client_table[src_idx].last_req = r_seqn;
                if (val > 0) {
                    client_table[src_idx].balance -= val;
                    if (dest_idx != -1 && dest_idx != src_idx) {
                        pthread_mutex_lock(&client_table[dest_idx].client_lock);
                        client_table[dest_idx].balance += val;
                        pthread_mutex_unlock(&client_table[dest_idx].client_lock);
                    }
                }
                
                // atualiza stats do backup para manter consistência
                pthread_mutex_lock(&stats_mutex);
                if (val > 0) {
                    num_transactions++; 
                    total_transferred += val;
                }
                total_balance = 0;
                for(int k=0; k<num_clients; k++) total_balance += client_table[k].balance;
                pthread_mutex_unlock(&stats_mutex);
            }
            pthread_mutex_unlock(&client_table[src_idx].client_lock);
        }
        pthread_mutex_unlock(&client_table_mutex);
    }
    
    // --- REQUISIÇÃO DO CLIENTE ---
    else if (type == TYPE_REQ) {
        
        if (!is_leader) {
            // backup não responde clientes, mas pode enviar erro para forçar redescoberta
             packet error_pkt;
             memset(&error_pkt, 0, sizeof(packet));
             error_pkt.type = htons(TYPE_ERROR_REQ);
             sendto(sockfd_local, &error_pkt, sizeof(packet), 0, (const struct sockaddr *)&client_addr, len);
             free(arg);
             return NULL;
        }

        uint32_t seqn = ntohl(pkt.seqn);
        uint32_t value = ntohl(pkt.value);

        pthread_mutex_lock(&client_table_mutex);
        int origin_idx = find_client(&client_addr);
        int dest_idx = find_client_ip(pkt.dest_addr);
        pthread_mutex_unlock(&client_table_mutex);

        if (origin_idx == -1) { 
            packet error_pkt;
            memset(&error_pkt, 0, sizeof(packet));
            error_pkt.type = htons(TYPE_ERROR_REQ);
            sendto(sockfd_local, &error_pkt, sizeof(packet), 0, (const struct sockaddr *)&client_addr, len);
        } else {
            int lock1 = origin_idx;
            int lock2 = -1;
            if (dest_idx != -1 && dest_idx != origin_idx) {
                if (origin_idx < dest_idx) { lock1 = origin_idx; lock2 = dest_idx; } 
                else { lock1 = dest_idx; lock2 = origin_idx; }
            }

            pthread_mutex_lock(&client_table[lock1].client_lock);
            if (lock2 != -1) pthread_mutex_lock(&client_table[lock2].client_lock);
            
            uint32_t expected_seqn = client_table[origin_idx].last_req + 1;
            uint32_t current_balance = (uint32_t)client_table[origin_idx].balance;
            uint32_t new_balance = current_balance;

            if (seqn >= expected_seqn) {
                client_table[origin_idx].last_req = seqn;

                if (dest_idx == -1) {
                    // erro destino
                    packet error_pkt;
                    memset(&error_pkt, 0, sizeof(packet));
                    error_pkt.type = htons(TYPE_ERROR_REQ);
                    sendto(sockfd_local, &error_pkt, sizeof(packet), 0, (const struct sockaddr *)&client_addr, len);
                    replicate_transaction(client_addr.sin_addr, pkt.dest_addr, 0, seqn, current_balance);
                }
                else if (value == 0) {
                    // consulta
                    packet ack_pkt;
                    memset(&ack_pkt, 0, sizeof(packet));
                    ack_pkt.type = htons(TYPE_ACK_REQ);
                    ack_pkt.balance = htonl(current_balance); 
                    ack_pkt.seqn = htonl(seqn);
                    sendto(sockfd_local, &ack_pkt, sizeof(packet), 0, (const struct sockaddr *)&client_addr, len);
                    
                    replicate_transaction(client_addr.sin_addr, pkt.dest_addr, value, seqn, new_balance);
                    
                    get_current_time(time_str, sizeof(time_str));
                    snprintf(logbuf, sizeof(logbuf), "%s CONSULTA id %d bal %u", time_str, origin_idx, current_balance);
                    push_log(logbuf);

                } else {
                    // transferência
                    if (origin_idx != dest_idx && current_balance >= value) {
                        client_table[origin_idx].balance -= (int32_t)value;
                        client_table[dest_idx].balance += (int32_t)value;
                        new_balance = (uint32_t)client_table[origin_idx].balance;

                        pthread_mutex_lock(&stats_mutex);
                        num_transactions++;
                        total_transferred += value;
                        pthread_mutex_unlock(&stats_mutex);                 
                    }
                    
                    packet ack_pkt;
                    memset(&ack_pkt, 0, sizeof(packet));
                    ack_pkt.type = htons(TYPE_ACK_REQ);
                    ack_pkt.balance = htonl(new_balance);
                    ack_pkt.seqn = htonl(seqn);
                    sendto(sockfd_local, &ack_pkt, sizeof(packet), 0, (const struct sockaddr *)&client_addr, len);

                    // replicar para backups
                    replicate_transaction(client_addr.sin_addr, pkt.dest_addr, value, seqn, new_balance);
                    
                    get_current_time(time_str, sizeof(time_str));
                    snprintf(logbuf, sizeof(logbuf), "%s TRANSF %u from %d to %d", time_str, value, origin_idx, dest_idx);
                    push_log(logbuf);
                }

            } else {
                // DUP
                if (seqn <= client_table[origin_idx].last_req) {
                    packet ack_pkt;
                    memset(&ack_pkt, 0, sizeof(packet));
                    ack_pkt.type = htons(TYPE_ACK_REQ);
                    ack_pkt.balance = htonl((uint32_t)client_table[origin_idx].balance);
                    ack_pkt.seqn = htonl(client_table[origin_idx].last_req);
                    sendto(sockfd_local, &ack_pkt, sizeof(packet), 0, (const struct sockaddr *)&client_addr, len);
                } 
            }

            if (lock2 != -1) pthread_mutex_unlock(&client_table[lock2].client_lock);
            pthread_mutex_unlock(&client_table[lock1].client_lock);
        }
    }
    
    free(arg);
    return NULL;
}

int main(int argc, char *argv[]) {
    setbuf(stdout, NULL);
    // uso: ./servidor <porta> <meu_id> <id_replica1> <ip_replica1> <porta_replica1> ...
    if (argc < 3) {
        fprintf(stderr, "Uso: ./servidor <porta> <meu_id> [id_rep ip_rep porta_rep ...]\n");
        return 1;
    }

    int port = atoi(argv[1]);
    my_id = atoi(argv[2]);
    
    is_leader = false; 
    current_leader_id = -1;

    printf("Iniciando Servidor ID %d na porta %d.\n", my_id, port);

    // parse dos argumentos das replicas
    int arg_idx = 3;
    while(arg_idx < argc - 2 && num_replicas < MAX_REPLICAS) {
        int r_id = atoi(argv[arg_idx]);
        char* ip_str = argv[arg_idx+1];
        int r_port = atoi(argv[arg_idx+2]);
        
        replicas[num_replicas].id = r_id; // Armazena ID
        replicas[num_replicas].active = true;
        replicas[num_replicas].addr.sin_family = AF_INET;
        replicas[num_replicas].addr.sin_port = htons(r_port);
        inet_aton(ip_str, &replicas[num_replicas].addr.sin_addr);
        
        printf("Replica Vizinha: ID %d IP %s Port %d\n", r_id, ip_str, r_port);

        num_replicas++;
        arg_idx += 3;
    }

    struct sockaddr_in server_addr;
    
    if (pthread_mutex_init(&client_table_mutex, NULL) != 0 || 
            pthread_mutex_init(&stats_mutex, NULL) != 0 ||
            pthread_mutex_init(&log_mutex, NULL) != 0 ||
            pthread_cond_init(&update_cond, NULL) != 0) {
            perror("falha ao inicializar mutexes/cond globais.\n");
            exit(EXIT_FAILURE);
    }

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

    // inicia thread de monitoramento
    pthread_t mon_tid;
    if (pthread_create(&mon_tid, NULL, monitor_thread, NULL) != 0) {
        perror("falha ao criar thread de monitoramento");
        exit(EXIT_FAILURE);
    }
    pthread_detach(mon_tid);

    // dispara eleição inicial
    pthread_t initial_elect_tid;
    if (pthread_create(&initial_elect_tid, NULL, (void*)start_election, NULL) != 0) {
        perror("falha ao criar thread de eleicao inicial");
    }
    pthread_detach(initial_elect_tid);
    
    while(1) {
        struct sockaddr_in client_addr_temp;    
        packet pkt_temp;                        
        socklen_t len = sizeof(client_addr_temp);
        
        int n = recvfrom(sockfd, &pkt_temp, sizeof(packet), 0, (struct sockaddr *)&client_addr_temp, &len);
        
        if (n>0) {
            request_data* data = (request_data*)malloc(sizeof(request_data));
            if (data == NULL) { continue; }
            
            data->pkt = pkt_temp;
            data->client_addr = client_addr_temp;
            data->len = len;
            data->sockfd = sockfd;      
            
            pthread_t thread_id;
            if (pthread_create(&thread_id, NULL, process_request, (void*)data) != 0) {
                free(data);
            }
            pthread_detach(thread_id);
        }
    }

    close(sockfd);
    //cleanups
    for (int i = 0; i < num_clients; i++) {
        pthread_mutex_destroy(&client_table[i].client_lock);
    }
    pthread_mutex_lock(&log_mutex);
    while (log_head != NULL) {
        log_node_t *temp = log_head;
        log_head = log_head->next;
        free(temp);
    }
    pthread_mutex_unlock(&log_mutex);
    pthread_mutex_destroy(&client_table_mutex);
    pthread_mutex_destroy(&stats_mutex);
    pthread_mutex_destroy(&log_mutex);
    pthread_mutex_destroy(&election_mutex);
    pthread_cond_destroy(&update_cond);
    return 0;
}