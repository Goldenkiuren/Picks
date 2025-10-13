// state.h — Estruturas e operações de estado (tabela de clientes e totais)
// Disciplina INF01151 — Etapa 1
// Mantém um "banco" em memória: clientes, saldos e totais.

#ifndef STATE_H
#define STATE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

    typedef struct Client Client; // forward declaration (lista encadeada interna)

    // getters/setters para acesso opaco a Client
    uint32_t client_get_last_req(const Client *c);
    void client_set_last_req(Client *c, uint32_t v);

    int64_t client_get_balance(const Client *c);
    void client_add_balance(Client *c, int64_t delta); // delta pode ser negativo

    // Snapshot imutável para testes e leitura
    typedef struct ClientSnapshot
    {
        bool exists;       // cliente existe?
        uint32_t ip_be;    // IPv4 em ordem de rede (big-endian)
        uint32_t last_req; // último id processado
        int64_t balance;   // saldo atual
    } ClientSnapshot;

    // Estado global do "banco"
    typedef struct BankState
    {
        Client *head;               // lista encadeada de clientes
        uint64_t num_transactions;  // total de transações efetivadas
        uint64_t total_transferred; // soma dos valores transferidos
        uint64_t total_balance;     // soma dos saldos de todos os clientes
    } BankState;

    // Inicializa, destrói e reseta o estado (para uso em testes)
    void state_init(BankState *st);
    void state_destroy(BankState *st);
    void state_reset(BankState *st);

    // Utilitário de inspeção (para testes)
    ClientSnapshot snapshot_client(BankState *st, uint32_t ip_be);

    // API interna — usada por logic.c
    Client *state_find_client(BankState *st, uint32_t ip_be);
    Client *state_insert_client(BankState *st, uint32_t ip_be, int64_t initial_balance);

#ifdef __cplusplus
}
#endif

#endif // STATE_H