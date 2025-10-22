#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>
#include <arpa/inet.h>

#define TYPE_DESCOBERTA 1
#define TYPE_ACK_DESCOBERTA 2
#define TYPE_REQ 3
#define TYPE_ACK_REQ 4
#define TYPE_ERROR_REQ 5 

#define SALDO_INICIAL 100


typedef struct {
    uint16_t type;          // tipo de pacote
    uint32_t seqn;         // número de sequência
    struct in_addr dest_addr;   // endereço de ip do cliente destino
    uint32_t value;       // valor de transferencia
    uint32_t balance;     // para ACKs, novo saldo      
} packet;

typedef struct {
    struct in_addr client_ip;   //endereço ip do cliente
    uint32_t last_req;          // id da ultima requisicao
    int32_t balance;
} client_data;


#endif