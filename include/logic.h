// include/logic.h

// logic.h — Regras de negócio do servidor (ordem, duplicata, saldo, rejeição)
// Disciplina INF01151 — Etapa 1
// Mantém a lógica **pura** para facilitar testes unitários.

#ifndef LOGIC_H
#define LOGIC_H

#include <stdint.h>
#include <stdbool.h>
#include "state.h"
#include <stdlib.h>

#ifdef __cplusplus
extern "C"
{
#endif

    // Resultado padronizado da lógica (espelha o que vai para o ACK e para o log)
    typedef struct LogicResult
    {
        bool processed;       // a requisição foi tratada (mesmo se "negada")
        bool duplicate;       // era duplicata (não reprocessada)
        uint32_t ack_seq;     // seq confirmado no ACK
        uint32_t new_balance; // saldo da origem após o tratamento
        // Totais após o tratamento (snapshot)
        uint64_t num_transactions;
        uint64_t total_transferred;
        uint64_t total_balance; // permanece constante em transferências internas
    } LogicResult;

    // Cadastro via descoberta (DESC)
    void handle_discovery_on_state(BankState *st, uint32_t ip_be);

    // Processamento de uma REQ (regra de negócio pura)
    LogicResult process_request_on_state(BankState *st,
                                         uint32_t orig_ip_be,
                                         uint32_t seqn,
                                         uint32_t dest_ip_be,
                                         uint32_t value);

#endif // LOGIC_H