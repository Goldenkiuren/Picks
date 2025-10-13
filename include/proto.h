// proto.h — Protocolo binário usado em UDP
// Estruturas e enums usados para (de)serializar mensagens entre cliente/servidor.

#ifndef PROTO_H
#define PROTO_H

#include <stdint.h>
#include <stddef.h>

// Tipos de pacote (ordem de rede na "wire")
enum PacketType
{
    PT_DESC = 1,     // descoberta (cliente -> servidor)
    PT_REQ = 2,      // requisição de transferência (cliente -> servidor)
    PT_DESC_ACK = 3, // resposta descoberta (servidor -> cliente)
    PT_REQ_ACK = 4   // ack requisição (servidor -> cliente)
};

#pragma pack(push, 1)
// Corpo da REQ
typedef struct
{
    uint32_t dest_addr; // IPv4 destino (ordem de rede)
    uint32_t value;     // valor (ordem de rede)
} requisicao_t;

// Corpo do ACK
typedef struct
{
    uint32_t seqn;        // id confirmado (ordem de rede)
    uint32_t new_balance; // novo saldo (ordem de rede)
} requisicao_ack_t;

// Pacote completo
typedef struct
{
    uint16_t type; // PacketType (ordem de rede)
    uint32_t seqn; // id da requisição (ordem de rede)
    union
    {
        requisicao_t req;
        requisicao_ack_t ack;
    } body;
} packet_t;
#pragma pack(pop)

// Tamanho mínimo para validar o cabeçalho
static inline size_t proto_min_header_size(void)
{
    return sizeof(uint16_t) + sizeof(uint32_t);
}

#endif // PROTO_H