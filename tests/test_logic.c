// tests/test_logic.c — Testes unitários (cmocka) para logic/state
// Como rodar:
//   make tests

#include <stdarg.h>
#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <cmocka.h>

#include "state.h"
#include "logic.h"

// Helper: cria IPv4 em ordem de rede a.b.c.d
static uint32_t BE4(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    uint32_t host = ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | ((uint32_t)d);
    return htonl(host);
}

// Fixture: cria/limpa estado por teste
static int setup_state(void **out)
{
    BankState *st = (BankState *)calloc(1, sizeof(BankState));
    state_init(st);
    *out = st;
    return 0;
}

static int teardown_state(void **stp)
{
    BankState *st = (BankState *)(*stp);
    state_destroy(st);
    free(st);
    return 0;
}

// 1) transferência OK
static void test_transfer_ok(void **stp)
{
    BankState *st = (BankState *)(*stp);
    uint32_t A = BE4(10, 1, 1, 2), B = BE4(10, 1, 1, 3);
    handle_discovery_on_state(st, A);
    handle_discovery_on_state(st, B);

    LogicResult r = process_request_on_state(st, A, 1, B, 10);

    assert_true(r.processed);
    assert_false(r.duplicate);
    assert_int_equal(r.ack_seq, 1);
    assert_int_equal(r.new_balance, 90);
    assert_int_equal(r.num_transactions, 1);
    assert_int_equal(r.total_transferred, 10);
    assert_int_equal(r.total_balance, 200);

    ClientSnapshot sa = snapshot_client(st, A);
    ClientSnapshot sb = snapshot_client(st, B);
    assert_true(sa.exists && sb.exists);
    assert_int_equal(sa.balance, 90);
    assert_int_equal(sb.balance, 110);
}

// 2) duplicata não tem efeito
static void test_duplicate_no_effect(void **stp)
{
    BankState *st = (BankState *)(*stp);
    uint32_t A = BE4(10, 1, 1, 2), B = BE4(10, 1, 1, 3);
    handle_discovery_on_state(st, A);
    handle_discovery_on_state(st, B);

    (void)process_request_on_state(st, A, 1, B, 10);
    LogicResult r2 = process_request_on_state(st, A, 1, B, 10); // duplicata

    assert_false(r2.processed);
    assert_true(r2.duplicate);
    assert_int_equal(r2.ack_seq, 1);
    assert_int_equal(r2.new_balance, 90);
    assert_int_equal(r2.num_transactions, 1); // não incrementou
    assert_int_equal(r2.total_transferred, 10);
}

// 3) fora de ordem devolve last
static void test_out_of_order_ack_last(void **stp)
{
    BankState *st = (BankState *)(*stp);
    uint32_t A = BE4(10, 1, 1, 2), B = BE4(10, 1, 1, 3);
    handle_discovery_on_state(st, A);
    handle_discovery_on_state(st, B);

    (void)process_request_on_state(st, A, 1, B, 5);
    LogicResult r = process_request_on_state(st, A, 3, B, 5); // pulou 2

    assert_false(r.processed);
    assert_false(r.duplicate);
    assert_int_equal(r.ack_seq, 1);
    assert_int_equal(r.new_balance, 95);
    assert_int_equal(r.num_transactions, 1);
    assert_int_equal(r.total_transferred, 5);
}

// 4) saldo insuficiente
static void test_insufficient_balance(void **stp)
{
    BankState *st = (BankState *)(*stp);
    uint32_t A = BE4(10, 1, 1, 2), B = BE4(10, 1, 1, 3);
    handle_discovery_on_state(st, A);
    handle_discovery_on_state(st, B);

    LogicResult r = process_request_on_state(st, A, 1, B, 1000);
    assert_true(r.processed);
    assert_false(r.duplicate);
    assert_int_equal(r.ack_seq, 1);          // last_req avança
    assert_int_equal(r.new_balance, 100);    // saldo inalterado
    assert_int_equal(r.num_transactions, 0); // não contou transação
    assert_int_equal(r.total_transferred, 0);
    assert_int_equal(r.total_balance, 200);
}

// 5) destino não cadastrado → rejeita
static void test_dest_not_registered(void **stp)
{
    BankState *st = (BankState *)(*stp);
    uint32_t A = BE4(10, 1, 1, 2), B = BE4(10, 1, 1, 3);
    handle_discovery_on_state(st, A);
    // B não cadastrado

    LogicResult r = process_request_on_state(st, A, 1, B, 10);
    assert_true(r.processed);
    assert_false(r.duplicate);
    assert_int_equal(r.ack_seq, 1);       // last_req avança
    assert_int_equal(r.new_balance, 100); // saldo inalterado
    assert_int_equal(r.num_transactions, 0);
    assert_int_equal(r.total_transferred, 0);
    assert_int_equal(r.total_balance, 100);
}

// 6) consulta de saldo (value=0)
static void test_balance_query_value_zero(void **stp)
{
    BankState *st = (BankState *)(*stp);
    uint32_t A = BE4(10, 1, 1, 2), B = BE4(10, 1, 1, 3);
    handle_discovery_on_state(st, A);
    handle_discovery_on_state(st, B);

    LogicResult r = process_request_on_state(st, A, 1, B, 0);
    assert_true(r.processed);
    assert_false(r.duplicate);
    assert_int_equal(r.ack_seq, 1);
    assert_int_equal(r.new_balance, 100);
    assert_int_equal(r.num_transactions, 0);
    assert_int_equal(r.total_transferred, 0);
    assert_int_equal(r.total_balance, 200);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_transfer_ok, setup_state, teardown_state),
        cmocka_unit_test_setup_teardown(test_duplicate_no_effect, setup_state, teardown_state),
        cmocka_unit_test_setup_teardown(test_out_of_order_ack_last, setup_state, teardown_state),
        cmocka_unit_test_setup_teardown(test_insufficient_balance, setup_state, teardown_state),
        cmocka_unit_test_setup_teardown(test_dest_not_registered, setup_state, teardown_state),
        cmocka_unit_test_setup_teardown(test_balance_query_value_zero, setup_state, teardown_state),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
