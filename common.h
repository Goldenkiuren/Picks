#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>
#include <arpa/inet.h>
#include <pthread.h>

#define TYPE_DESCOBERTA 1
#define TYPE_ACK_DESCOBERTA 2
#define TYPE_REQ 3
#define TYPE_ACK_REQ 4
#define TYPE_ERROR_REQ 5 
#define TYPE_REPLICATION 6 

// Tipos para Eleição de Líder e Heartbeat
#define TYPE_HEARTBEAT 7    // Líder diz "estou vivo"
#define TYPE_ELECTION 8     // "Eu quero ser líder" (para IDs maiores)
#define TYPE_ANSWER 9       // "Eu sou maior, cale-se" (resposta ao Election)
#define TYPE_COORDINATOR 10 // "Eu venci, sou o novo líder"

#define SALDO_INICIAL 100

typedef struct {
    uint16_t type;          // tipo de pacote
    uint32_t seqn;          // número de sequência
    struct in_addr dest_addr;   
    struct in_addr src_addr;
    uint32_t value;         // valor de transferencia
    uint32_t balance;       // para ACKs, novo saldo      
} packet;

typedef struct {
    struct in_addr client_ip;
    int32_t balance;
    uint32_t last_req;
} replication_payload;

typedef struct {
    struct in_addr client_ip;   
    uint32_t last_req;          
    int32_t balance;
    pthread_mutex_t client_lock;
} client_data;

#endif