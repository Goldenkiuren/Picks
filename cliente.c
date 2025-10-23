#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <sys/select.h>  // FIX: Adicionado pra timeout com select
#include "common.h"

#define BROADCAST_IP "255.255.255.255"

int main(int argc, char *argv[]) {
    
    if (argc != 2) {
        fprintf(stderr, "Use: ./cliente <porta>\n");
        return 1;
    }

    int port = atoi(argv[1]);
    int sockfd;
    struct sockaddr_in server_addr, broadcast_addr;
    packet discovery_pkt, response_pkt;

    //criando socket UDP
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("falha na criação do socket.");
        exit(EXIT_FAILURE);
    }

    // habilita broadcast no socket
    int broadcast_enable = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_BROADCAST, &broadcast_enable, sizeof(broadcast_enable)) < 0) {
        perror("falha ao habilitar broadcast");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    memset(&broadcast_addr, 0, sizeof(broadcast_addr));

    broadcast_addr.sin_family = AF_INET;
    broadcast_addr.sin_port = htons(port);

    // configurando o endereço do broadcast
    if (inet_aton(BROADCAST_IP, &broadcast_addr.sin_addr) == 0) {
        fprintf(stderr, "Endereco IP invalido\n");
        exit(EXIT_FAILURE);
    }

    // prepara e envia o pacote de descoberta
    memset(&discovery_pkt, 0, sizeof(packet));  // Inicializa pra evitar lixo
    discovery_pkt.type = htons(TYPE_DESCOBERTA);  // Host to Network
    printf("Enviando pacote de descoberta... \n");
    sendto(sockfd, &discovery_pkt, sizeof(packet), 0, (const struct sockaddr *)&broadcast_addr, sizeof(broadcast_addr));

    printf("Aguardando resposta do servidor...\n");
    socklen_t len = sizeof(server_addr);
    int n = recvfrom(sockfd, &response_pkt, sizeof(packet), 0, (struct sockaddr *)&server_addr, &len);

    // aguarda resposta do servidor - JÁ CORRIGIDO: ntohs pro type (evita 2 virar 512)
    if (n > 0 && ntohs(response_pkt.type) == TYPE_ACK_DESCOBERTA)  {
        char time_buffer[100];
        time_t now = time(0);
        struct tm *t = localtime(&now);
        strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d %H:%M:%S", t);

        printf("%s server_addr %s\n", time_buffer, inet_ntoa(server_addr.sin_addr));

        uint32_t seqn_local = 0;
        char ip_str[20];
        uint32_t valor;

        // loop de leitura de comandos
        while (scanf("%s %u", ip_str, &valor) == 2) {
            // Validação de IP
            struct in_addr temp_addr;
            if (inet_aton(ip_str, &temp_addr) == 0) {
                fprintf(stderr, "IP inválido: %s\n", ip_str);
                continue;
            }

            seqn_local++;

            //pacote de requisicao
            packet req_pkt;
            memset(&req_pkt, 0, sizeof(packet));  // Inicializa
            req_pkt.type = htons(TYPE_REQ);  // Host to Network
            req_pkt.seqn = htonl(seqn_local);
            req_pkt.value = htonl(valor);
            inet_aton(ip_str, &req_pkt.dest_addr); 

            //envia requisicao
            sendto(sockfd, &req_pkt, sizeof(packet), 0, (const struct sockaddr *)&server_addr, sizeof(server_addr));

            //aguarda resposta - COM TIMEOUT
            packet ack_pkt;
            struct timeval timeout;
            timeout.tv_sec = 5;  // 5s timeout
            timeout.tv_usec = 0;
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(sockfd, &readfds);
            int ready = select(sockfd + 1, &readfds, NULL, NULL, &timeout);
            if (ready > 0) {
                n = recvfrom(sockfd, &ack_pkt, sizeof(packet), 0, NULL, NULL);
                // JÁ CORRIGIDO: ntohs/ntohl pro type, seqn e balance (evita valores gigantes como 1024 ou 16777216)
                if (n > 0 && ntohs(ack_pkt.type) == TYPE_ACK_REQ && ntohl(ack_pkt.seqn) == seqn_local) {
                    now = time(0);
                    t = localtime(&now);
                    strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d %H:%M:%S", t);

                    // imprime resultado - JÁ CORRIGIDO: ntohl pro balance (e seqn_local pro id req)
                    printf("%s server %s id req %u dest %s value %u new_balance %u\n", time_buffer, inet_ntoa(server_addr.sin_addr), seqn_local, ip_str, valor, ntohl(ack_pkt.balance));
                } else if (ntohs(ack_pkt.type) == TYPE_ERROR_REQ) {  // ADICIONADO: Tratamento pro erro do servidor
                    fprintf(stderr, "Erro no servidor: Requisição inválida (ex.: cliente destino não encontrado).\n");
                } else {
                    // timeouts e retransmissao
                    fprintf(stderr, "Erro: ACK não recebido ou pacote inválido (type: %d, seqn: %u vs esperado %u).\n", ntohs(ack_pkt.type), ntohl(ack_pkt.seqn), seqn_local);
                }
            } else {
                fprintf(stderr, "Timeout na recepção de ACK. Pacote perdido.\n");
            }
        }
    } else {
        printf("Nenhuma resposta do servidor recebida.\n");
    }
    

    close(sockfd);
    return 0;
}
