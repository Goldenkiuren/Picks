// servidor.c
// INF01151 - TP Etapa 1 - Servidor UDP (C, pthreads)
// gcc -O2 -pthread servidor.c -o servidor

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define INITIAL_BALANCE 100
#define POOL_SIZE 4
#define QUEUE_CAP 1024

// ===== Protocolo =====
enum PacketType : uint16_t
{
    DESC = 1,     // descoberta (cliente -> servidor)
    REQ = 2,      // requisição de transferência (cliente -> servidor)
    DESC_ACK = 3, // resposta descoberta (servidor -> cliente)
    REQ_ACK = 4   // ack requisição (servidor -> cliente)
};

#pragma pack(push, 1)
typedef struct
{
    uint32_t dest_addr; // IPv4 destino (ordem de rede)
    uint32_t value;     // valor (ordem de rede)
} requisicao_t;

typedef struct
{
    uint32_t seqn;        // id confirmado (ordem de rede)
    uint32_t new_balance; // novo saldo origem (ordem de rede)
} requisicao_ack_t;

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

// ===== Utilidades =====
static void now_prefix(FILE *out)
{
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
    fprintf(out, "%s ", buf);
}

static const char *ip_str(uint32_t ip_be, char *buf, size_t n)
{
    struct in_addr in;
    in.s_addr = ip_be; // já em ordem de rede
    const char *s = inet_ntop(AF_INET, &in, buf, (socklen_t)n);
    return s ? s : "0.0.0.0";
}

// ===== Estruturas de estado =====
typedef struct Client
{
    uint32_t ip;       // em ordem de rede (big-endian)
    uint32_t last_req; // último id processado
    int64_t balance;   // saldo atual
    // para o log de duplicatas:
    uint32_t last_log_seq;
    uint32_t last_log_dest; // ordem de rede
    uint32_t last_log_value;
    struct Client *next;
} Client;

typedef struct
{
    // lista de clientes
    Client *head;
    // totais globais
    uint64_t num_transactions;
    uint64_t total_transferred;
    uint64_t total_balance; // soma de todos os saldos (muda quando entra cliente novo)
    // sincronização leitor/escritor
    pthread_rwlock_t rw;
} BankState;

static BankState G = {
    .head = NULL,
    .num_transactions = 0,
    .total_transferred = 0,
    .total_balance = 0,
    .rw = PTHREAD_RWLOCK_INITIALIZER};

// busca/insere cliente (com WR-lock já adquirido pelo chamador)
static Client *find_client(uint32_t ip_be)
{
    for (Client *c = G.head; c; c = c->next)
        if (c->ip == ip_be)
            return c;
    return NULL;
}

static Client *insert_client(uint32_t ip_be)
{
    Client *c = (Client *)calloc(1, sizeof(Client));
    c->ip = ip_be;
    c->last_req = 0;
    c->balance = INITIAL_BALANCE;
    c->last_log_seq = 0;
    c->last_log_dest = 0;
    c->last_log_value = 0;
    c->next = G.head;
    G.head = c;
    G.total_balance += INITIAL_BALANCE; // entra no sistema
    return c;
}

// ===== Tarefas e thread pool =====
typedef struct
{
    packet_t pkt;
    struct sockaddr_in src;
    socklen_t srclen;
} Task;

typedef struct
{
    Task buf[QUEUE_CAP];
    int head, tail, size;
    pthread_mutex_t mtx;
    pthread_cond_t cv_nonempty;
    pthread_cond_t cv_nonfull;
} TaskQ;

static TaskQ Q = {
    .head = 0, .tail = 0, .size = 0, .mtx = PTHREAD_MUTEX_INITIALIZER, .cv_nonempty = PTHREAD_COND_INITIALIZER, .cv_nonfull = PTHREAD_COND_INITIALIZER};

static void q_push(const Task *t)
{
    pthread_mutex_lock(&Q.mtx);
    while (Q.size == QUEUE_CAP)
        pthread_cond_wait(&Q.cv_nonfull, &Q.mtx);
    Q.buf[Q.tail] = *t;
    Q.tail = (Q.tail + 1) % QUEUE_CAP;
    Q.size++;
    pthread_cond_signal(&Q.cv_nonempty);
    pthread_mutex_unlock(&Q.mtx);
}

static Task q_pop(void)
{
    pthread_mutex_lock(&Q.mtx);
    while (Q.size == 0)
        pthread_cond_wait(&Q.cv_nonempty, &Q.mtx);
    Task t = Q.buf[Q.head];
    Q.head = (Q.head + 1) % QUEUE_CAP;
    Q.size--;
    pthread_cond_signal(&Q.cv_nonfull);
    pthread_mutex_unlock(&Q.mtx);
    return t;
}

// ===== Socket global (envio de ACK) =====
static int G_sock = -1;

// ===== Envio de ACK =====
static void send_desc_ack(const struct sockaddr_in *dst, socklen_t dstlen)
{
    packet_t resp;
    memset(&resp, 0, sizeof(resp));
    resp.type = htons(DESC_ACK);
    resp.seqn = htonl(0);
    // enviar apenas cabeçalho {type, seqn} é suficiente
    if (sendto(G_sock, &resp, sizeof(resp.type) + sizeof(resp.seqn), 0,
               (const struct sockaddr *)dst, dstlen) < 0)
    {
        perror("sendto DESC_ACK");
    }
}

static void send_req_ack(const struct sockaddr_in *dst, socklen_t dstlen,
                         uint32_t ack_seq_host, uint32_t new_balance_host)
{
    packet_t ack;
    memset(&ack, 0, sizeof(ack));
    ack.type = htons(REQ_ACK);
    ack.seqn = htonl(ack_seq_host);
    ack.body.ack.seqn = htonl(ack_seq_host);
    ack.body.ack.new_balance = htonl(new_balance_host);
    if (sendto(G_sock, &ack, sizeof(ack), 0,
               (const struct sockaddr *)dst, dstlen) < 0)
    {
        perror("sendto REQ_ACK");
    }
}

// ===== Log helpers =====
static void log_processed(uint32_t src_ip_be, uint32_t seqn,
                          uint32_t dest_ip_be, uint32_t value,
                          int dup_flag)
{
    // imprime 2 ou 3 linhas conforme o enunciado
    char srcs[INET_ADDRSTRLEN], dsts[INET_ADDRSTRLEN];
    ip_str(src_ip_be, srcs, sizeof(srcs));
    ip_str(dest_ip_be, dsts, sizeof(dsts));

    now_prefix(stdout);
    if (dup_flag)
    {
        printf("client %s DUP!! id req %u dest %s value %u\n",
               srcs, seqn, dsts, value);
        printf("num transactions %llu total transferred %llu total balance %llu\n",
               (unsigned long long)G.num_transactions,
               (unsigned long long)G.total_transferred,
               (unsigned long long)G.total_balance);
    }
    else
    {
        printf("client %s id req %u dest %s value %u\n",
               srcs, seqn, dsts, value);
        printf("num transactions %llu\n", (unsigned long long)G.num_transactions);
        printf("total transferred %llu total balance %llu\n",
               (unsigned long long)G.total_transferred,
               (unsigned long long)G.total_balance);
    }
}

// ===== Processamento de mensagens =====
static void handle_desc(const Task *t)
{
    // writer: inserir cliente se não existir
    pthread_rwlock_wrlock(&G.rw);
    Client *c = find_client(t->src.sin_addr.s_addr);
    if (!c)
        insert_client(t->src.sin_addr.s_addr);
    pthread_rwlock_unlock(&G.rw);

    // responde DESC_ACK
    send_desc_ack(&t->src, t->srclen);

    // log opcional de debug
    char s[INET_ADDRSTRLEN];
    now_prefix(stdout);
    printf("desc from %s -> DESC_ACK sent\n",
           ip_str(t->src.sin_addr.s_addr, s, sizeof(s)));
}

static void handle_req(const Task *t)
{
    // extrai campos
    uint32_t seqn = ntohl(t->pkt.seqn);
    uint32_t dest_be = t->pkt.body.req.dest_addr; // já em rede
    uint32_t value = ntohl(t->pkt.body.req.value);
    uint32_t src_be = t->src.sin_addr.s_addr;

    // regras
    pthread_rwlock_wrlock(&G.rw);

    Client *src = find_client(src_be);
    if (!src)
    {
        // origem não cadastrada → informa last_seq=0, saldo 0
        pthread_rwlock_unlock(&G.rw);
        send_req_ack(&t->src, t->srclen, 0, 0);
        return;
    }

    uint32_t k = src->last_req;

    if (seqn == k + 1)
    {
        // possível nova operação
        if (value == 0)
        {
            // consulta de saldo (não altera totais)
            src->last_req = seqn;
            uint32_t nb = (uint32_t)(src->balance < 0 ? 0 : src->balance);
            // atualiza "último log" para DUP!!
            src->last_log_seq = seqn;
            src->last_log_dest = dest_be;
            src->last_log_value = value;
            // snapshot para log fora do lock
            uint64_t nt = G.num_transactions;
            uint64_t tt = G.total_transferred;
            uint64_t tb = G.total_balance;
            pthread_rwlock_unlock(&G.rw);

            // imprime no formato (contadores não mudam)
            now_prefix(stdout);
            char dsts[INET_ADDRSTRLEN], srcs[INET_ADDRSTRLEN];
            ip_str(src_be, srcs, sizeof(srcs));
            ip_str(dest_be, dsts, sizeof(dsts));
            printf("client %s id req %u dest %s value %u\n",
                   srcs, seqn, dsts, value);
            printf("num transactions %llu\n", (unsigned long long)nt);
            printf("total transferred %llu total balance %llu\n",
                   (unsigned long long)tt, (unsigned long long)tb);

            send_req_ack(&t->src, t->srclen, seqn, nb);
            return;
        }

        // precisa existir destino
        Client *dst = find_client(dest_be);
        if (!dst)
        {
            // política: rejeita se destino não cadastrado
            src->last_req = seqn;
            uint32_t nb = (uint32_t)(src->balance < 0 ? 0 : src->balance);
            // guardar log "negada": vamos registrar com value=0
            src->last_log_seq = seqn;
            src->last_log_dest = dest_be;
            src->last_log_value = 0;

            uint64_t nt = G.num_transactions;
            uint64_t tt = G.total_transferred;
            uint64_t tb = G.total_balance;
            pthread_rwlock_unlock(&G.rw);

            // imprime (sem alterar contadores)
            now_prefix(stdout);
            char dsts[INET_ADDRSTRLEN], srcs[INET_ADDRSTRLEN];
            ip_str(src_be, srcs, sizeof(srcs));
            ip_str(dest_be, dsts, sizeof(dsts));
            printf("client %s id req %u dest %s value %u\n",
                   srcs, seqn, dsts, 0u);
            printf("num transactions %llu\n", (unsigned long long)nt);
            printf("total transferred %llu total balance %llu\n",
                   (unsigned long long)tt, (unsigned long long)tb);

            send_req_ack(&t->src, t->srclen, seqn, nb);
            return;
        }

        // checa saldo
        if ((uint64_t)src->balance < (uint64_t)value)
        {
            // saldo insuficiente: registra last_req, counters inalterados
            src->last_req = seqn;
            uint32_t nb = (uint32_t)(src->balance < 0 ? 0 : src->balance);
            // log com value=0 para indicar que não transferiu nada
            src->last_log_seq = seqn;
            src->last_log_dest = dest_be;
            src->last_log_value = 0;

            uint64_t nt = G.num_transactions;
            uint64_t tt = G.total_transferred;
            uint64_t tb = G.total_balance;
            pthread_rwlock_unlock(&G.rw);

            now_prefix(stdout);
            char dsts[INET_ADDRSTRLEN], srcs[INET_ADDRSTRLEN];
            ip_str(src_be, srcs, sizeof(srcs));
            ip_str(dest_be, dsts, sizeof(dsts));
            printf("client %s id req %u dest %s value %u\n",
                   srcs, seqn, dsts, 0u);
            printf("num transactions %llu\n", (unsigned long long)nt);
            printf("total transferred %llu total balance %llu\n",
                   (unsigned long long)tt, (unsigned long long)tb);

            send_req_ack(&t->src, t->srclen, seqn, nb);
            return;
        }

        // executa transferência
        src->balance -= (int64_t)value;
        dst->balance += (int64_t)value;
        src->last_req = seqn;

        G.num_transactions += 1;
        G.total_transferred += value;
        // G.total_balance inalterado (transferência interna)

        // guarda última linha para DUP!!
        src->last_log_seq = seqn;
        src->last_log_dest = dest_be;
        src->last_log_value = value;

        uint64_t nt = G.num_transactions;
        uint64_t tt = G.total_transferred;
        uint64_t tb = G.total_balance;
        uint32_t nb = (uint32_t)(src->balance < 0 ? 0 : src->balance);

        // snapshot para log fora do lock
        pthread_rwlock_unlock(&G.rw);

        // imprime no formato exigido
        log_processed(src_be, seqn, dest_be, value, /*dup=*/0);
        // envia ACK
        send_req_ack(&t->src, t->srclen, seqn, nb);
        return;
    }
    else if (seqn <= k)
    {
        // DUPLICATA → reimprime última mensagem + ACK com k
        // precisamos do último log salvo
        uint32_t last_seq = src->last_log_seq;
        uint32_t last_dst = src->last_log_dest;
        uint32_t last_val = src->last_log_value;
        uint64_t nt = G.num_transactions;
        uint64_t tt = G.total_transferred;
        uint64_t tb = G.total_balance;
        uint32_t nb = (uint32_t)(src->balance < 0 ? 0 : src->balance);
        pthread_rwlock_unlock(&G.rw);

        // reimprime como DUP!!
        // (usamos os dados do "último processado" do cliente)
        if (last_seq != 0)
        {
            // usa o último log
            pthread_rwlock_rdlock(&G.rw);
            log_processed(src_be, last_seq, last_dst, last_val, /*dup=*/1);
            pthread_rwlock_unlock(&G.rw);
        }
        else
        {
            // fallback: imprime algo simples
            now_prefix(stdout);
            char srcs[INET_ADDRSTRLEN];
            ip_str(src_be, srcs, sizeof(srcs));
            printf("client %s DUP!! id req %u dest 0.0.0.0 value 0\n", srcs, seqn);
            printf("num transactions %llu total transferred %llu total balance %llu\n",
                   (unsigned long long)nt, (unsigned long long)tt, (unsigned long long)tb);
        }

        send_req_ack(&t->src, t->srclen, k, nb);
        return;
    }
    else
    { // seqn > k+1
        // fora de ordem: não processa; informa último aceito (k)
        uint32_t nb = (uint32_t)(src->balance < 0 ? 0 : src->balance);
        pthread_rwlock_unlock(&G.rw);
        send_req_ack(&t->src, t->srclen, k, nb);
        return;
    }
}

// ===== Worker threads =====
static void *worker_main(void *_)
{
    (void)_;
    for (;;)
    {
        Task t = q_pop();
        uint16_t type = ntohs(t.pkt.type);
        if (type == DESC)
            handle_desc(&t);
        else if (type == REQ)
            handle_req(&t);
        // demais tipos: ignorar
    }
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        fprintf(stderr, "Use: ./servidor <porta>\n");
        return 1;
    }
    int port = atoi(argv[1]);

    // socket
    G_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (G_sock < 0)
    {
        perror("socket");
        exit(1);
    }

    int yes = 1;
    setsockopt(G_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(G_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("bind");
        exit(1);
    }

    // imprime linha inicial exigida
    now_prefix(stdout);
    printf("num transactions %llu total transferred %llu total balance %llu\n",
           (unsigned long long)G.num_transactions,
           (unsigned long long)G.total_transferred,
           (unsigned long long)G.total_balance);

    // inicia pool
    pthread_t th[POOL_SIZE];
    for (int i = 0; i < POOL_SIZE; ++i)
        pthread_create(&th[i], NULL, worker_main, NULL);

    // loop de recepção → produz tarefas
    for (;;)
    {
        Task t;
        memset(&t, 0, sizeof(t));
        t.srclen = sizeof(t.src);
        ssize_t n = recvfrom(G_sock, &t.pkt, sizeof(t.pkt), 0,
                             (struct sockaddr *)&t.src, &t.srclen);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            perror("recvfrom");
            continue;
        }
        // valida tamanho mínimo {type, seqn}
        if (n < (ssize_t)(sizeof(uint16_t) + sizeof(uint32_t)))
            continue;
        // enfileira
        q_push(&t);
    }

    close(G_sock);
    return 0;
}
