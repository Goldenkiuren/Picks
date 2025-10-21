#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>

#define TYPE_DESCOBERTA 1
#define TYPE_ACK_DESCOBERTA 2

typedef struct {
    uint16_t type;          // tipo de pacote
    //uint32_t seqn;         // número de sequência
    //uint32_t dest_addr;   // endereço de ip do cliente destino
    //uint32_t value;       // valor de transferencia
    //uint32_t balance;     // para ACKs, novo saldo      
} packet;

#endif