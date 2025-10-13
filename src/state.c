// state.c — Implementação das estruturas e operações de estado
// Lista encadeada simples para clientes + totais globais

#include <stdlib.h>
#include <string.h>
#include "state.h"

struct Client
{
    uint32_t ip;       // IPv4 em ordem de rede (big-endian)
    uint32_t last_req; // último id processado
    int64_t balance;   // saldo atual
    struct Client *next;
};

uint32_t client_get_last_req(const Client *c)
{
    return c ? c->last_req : 0;
}
void client_set_last_req(Client *c, uint32_t v)
{
    if (c)
        c->last_req = v;
}
int64_t client_get_balance(const Client *c)
{
    return c ? c->balance : 0;
}
void client_add_balance(Client *c, int64_t delta)
{
    if (c)
        c->balance += delta;
}

void state_init(BankState *st)
{
    memset(st, 0, sizeof(*st));
}

static void free_list(Client *c)
{
    while (c)
    {
        Client *n = c->next;
        free(c);
        c = n;
    }
}

void state_destroy(BankState *st)
{
    free_list(st->head);
    memset(st, 0, sizeof(*st));
}

void state_reset(BankState *st)
{
    free_list(st->head);
    st->head = NULL;
    st->num_transactions = 0;
    st->total_transferred = 0;
    st->total_balance = 0;
}

Client *state_find_client(BankState *st, uint32_t ip_be)
{
    for (Client *c = st->head; c; c = c->next)
        if (c->ip == ip_be)
            return c;
    return NULL;
}

Client *state_insert_client(BankState *st, uint32_t ip_be, int64_t initial_balance)
{
    Client *c = (Client *)calloc(1, sizeof(Client));
    c->ip = ip_be;
    c->last_req = 0;
    c->balance = initial_balance;
    c->next = st->head;
    st->head = c;
    st->total_balance += (uint64_t)initial_balance; // entra no sistema
    return c;
}

ClientSnapshot snapshot_client(BankState *st, uint32_t ip_be)
{
    ClientSnapshot s = {0};
    for (Client *c = st->head; c; c = c->next)
    {
        if (c->ip == ip_be)
        {
            s.exists = true;
            s.ip_be = c->ip;
            s.last_req = c->last_req;
            s.balance = c->balance;
            break;
        }
    }
    return s;
}