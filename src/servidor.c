// servidor.c — main + socket UDP + logs + integração com a lógica
// Usa o protocolo binário definido em proto.h e a lógica pura (logic.c)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <time.h>

#include "servidor.h"
#include "proto.h"
#include "logic.h"
#include "state.h"

static void
now_prefix(FILE *out)
{
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
    fprintf(out, "%s ", buf);
}

int server_socket_init(int port)
{
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0)
    {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    int yes = 1;
    (void)setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("bind");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    printf("Servidor UDP escutando na porta %d\n", port);
    return sockfd;
}

static void send_desc_ack_fd(int sockfd, const struct sockaddr_in *dst, socklen_t dstlen)
{
    packet_t resp = {0};
    resp.type = htons(PT_DESC_ACK);
    resp.seqn = htonl(0);
    sendto(sockfd, &resp, sizeof(resp.type) + sizeof(resp.seqn), 0,
           (const struct sockaddr *)dst, dstlen);
}

static void send_req_ack_fd(int sockfd, const struct sockaddr_in *dst, socklen_t dstlen,
                            uint32_t ack_seq_host, uint32_t new_balance_host)
{
    packet_t ack = {0};
    ack.type = htons(PT_REQ_ACK);
    ack.seqn = htonl(ack_seq_host);
    ack.body.ack.seqn = htonl(ack_seq_host);
    ack.body.ack.new_balance = htonl(new_balance_host);
    sendto(sockfd, &ack, sizeof(ack), 0, (const struct sockaddr *)dst, dstlen);
}

void server_main_loop(int sockfd, BankState *state)
{
    struct sockaddr_in client_addr;
    socklen_t len = sizeof(client_addr);
    packet_t pkt;

    // Linha inicial exigida
    now_prefix(stdout);
    printf("num transactions %llu total transferred %llu total balance %llu\n",
           (unsigned long long)state->num_transactions,
           (unsigned long long)state->total_transferred,
           (unsigned long long)state->total_balance);

    for (;;)
    {
        ssize_t n = recvfrom(sockfd, &pkt, sizeof(pkt), 0,
                             (struct sockaddr *)&client_addr, &len);
        if (n < 0)
        {
            if (errno == EINTR)
                continue; // sinal
            perror("recvfrom");
            continue;
        }
        if ((size_t)n < proto_min_header_size())
            continue; // pacote muito curto

        uint16_t type = ntohs(pkt.type);
        uint32_t seqn = ntohl(pkt.seqn);

        if (type == PT_DESC)
        {
            // Cadastra cliente e responde DESC_ACK
            handle_discovery_on_state(state, client_addr.sin_addr.s_addr);
            send_desc_ack_fd(sockfd, &client_addr, len);

            // Log opcional (não exigido no formato)
            char ss[INET_ADDRSTRLEN];
            now_prefix(stdout);
            printf("desc from %s -> DESC_ACK sent\n",
                   inet_ntop(AF_INET, &client_addr.sin_addr, ss, sizeof(ss)));
        }
        else if (type == PT_REQ)
        {
            if ((size_t)n < sizeof(packet_t))
                continue;                              // precisamos do corpo completo
            uint32_t dest_be = pkt.body.req.dest_addr; // já em ordem de rede
            uint32_t value = ntohl(pkt.body.req.value);

            LogicResult r = process_request_on_state(state,
                                                     client_addr.sin_addr.s_addr,
                                                     seqn,
                                                     dest_be,
                                                     value);

            // Logs no formato do enunciado
            char srcs[INET_ADDRSTRLEN], dsts[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &client_addr.sin_addr, srcs, sizeof(srcs));
            struct in_addr din;
            din.s_addr = dest_be;
            inet_ntop(AF_INET, &din, dsts, sizeof(dsts));

            if (r.duplicate)
            {
                now_prefix(stdout);
                printf("client %s DUP!! id req %u dest %s value %u\n",
                       srcs, seqn, dsts, value);
                printf("num transactions %llu total transferred %llu total balance %llu\n",
                       (unsigned long long)r.num_transactions,
                       (unsigned long long)r.total_transferred,
                       (unsigned long long)r.total_balance);
            }
            else if (r.processed)
            {
                now_prefix(stdout);
                printf("client %s id req %u dest %s value %u\n",
                       srcs, seqn, dsts, value == 0 ? 0u : value);
                printf("num transactions %llu\n", (unsigned long long)r.num_transactions);
                printf("total transferred %llu total balance %llu\n",
                       (unsigned long long)r.total_transferred,
                       (unsigned long long)r.total_balance);
            }

            // ACK com seq confirmado e novo saldo
            send_req_ack_fd(sockfd, &client_addr, len, r.ack_seq, r.new_balance);
        }
    }
}

int main(int argc, char **argv)
{
    int port = (argc >= 2) ? atoi(argv[1]) : SERVER_PORT_DEFAULT;

    BankState state;
    state_init(&state);

    int sockfd = server_socket_init(port);
    server_main_loop(sockfd, &state);

    close(sockfd);
    state_destroy(&state);
    return 0;
}