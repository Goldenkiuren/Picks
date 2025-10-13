// logic.c — Implementação da regra de negócio
// Mantém a lógica pura (sem printf/sendto), retornando LogicResult

#include "logic.h"

#ifndef INITIAL_BALANCE
#define INITIAL_BALANCE 100
#endif

static LogicResult make_result(BankState *st, bool processed, bool duplicate,
                               uint32_t ack_seq, uint32_t new_balance)
{
    LogicResult r;
    r.processed = processed;
    r.duplicate = duplicate;
    r.ack_seq = ack_seq;
    r.new_balance = new_balance;
    r.num_transactions = st->num_transactions;
    r.total_transferred = st->total_transferred;
    r.total_balance = st->total_balance;
    return r;
}

void handle_discovery_on_state(BankState *st, uint32_t ip_be)
{
    if (!state_find_client(st, ip_be))
    {
        state_insert_client(st, ip_be, INITIAL_BALANCE);
    }
}

LogicResult process_request_on_state(BankState *st,
                                     uint32_t orig_ip_be,
                                     uint32_t seqn,
                                     uint32_t dest_ip_be,
                                     uint32_t value)
{
    Client *src = state_find_client(st, orig_ip_be);
    if (!src)
    {
        return make_result(st, false, false, 0, 0);
    }

    uint32_t k = client_get_last_req(src);

    if (seqn <= k)
    { // DUPLICATA
        int64_t bal = client_get_balance(src);
        uint32_t nb = (uint32_t)(bal < 0 ? 0 : bal);
        return make_result(st, false, true, k, nb);
    }

    if (seqn > k + 1)
    { // FORA DE ORDEM
        int64_t bal = client_get_balance(src);
        uint32_t nb = (uint32_t)(bal < 0 ? 0 : bal);
        return make_result(st, false, false, k, nb);
    }

    // value==0 → consulta de saldo
    if (value == 0)
    {
        client_set_last_req(src, seqn);
        int64_t bal = client_get_balance(src);
        uint32_t nb = (uint32_t)(bal < 0 ? 0 : bal);
        return make_result(st, true, false, seqn, nb);
    }

    // destino deve existir
    Client *dst = state_find_client(st, dest_ip_be);
    if (!dst)
    {
        client_set_last_req(src, seqn);
        int64_t bal = client_get_balance(src);
        uint32_t nb = (uint32_t)(bal < 0 ? 0 : bal);
        return make_result(st, true, false, seqn, nb);
    }

    // saldo suficiente?
    if ((uint64_t)client_get_balance(src) < (uint64_t)value)
    {
        client_set_last_req(src, seqn);
        int64_t bal = client_get_balance(src);
        uint32_t nb = (uint32_t)(bal < 0 ? 0 : bal);
        return make_result(st, true, false, seqn, nb);
    }

    // efetiva transferência
    client_add_balance(src, -(int64_t)value);
    client_add_balance(dst, +(int64_t)value);
    client_set_last_req(src, seqn);

    st->num_transactions += 1;
    st->total_transferred += (uint64_t)value;

    int64_t bal = client_get_balance(src);
    uint32_t nb = (uint32_t)(bal < 0 ? 0 : bal);
    return make_result(st, true, false, seqn, nb);
}