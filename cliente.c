#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
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
    discovery_pkt.type = TYPE_DESCOBERTA;
    printf("Enviando pacote de descoberta... \n");
    sendto(sockfd, &discovery_pkt, sizeof(packet), 0, (const struct sockaddr *)&broadcast_addr, sizeof(broadcast_addr));

    printf("Aguardando resposta do servidor...\n");
    socklen_t len = sizeof(server_addr);
    int n = recvfrom(sockfd, &response_pkt, sizeof(packet), 0, (struct sockaddr *)&server_addr, &len);

    // aguarda resposta do servidor
    if (n > 0 && response_pkt.type == TYPE_ACK_DESCOBERTA)  {
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
            seqn_local++;

            //pacote de requisicao
            packet req_pkt;
            req_pkt.type = TYPE_REQ;
            req_pkt.seqn = seqn_local;
            req_pkt.value = valor;
            inet_aton(ip_str, &req_pkt.dest_addr); 

            //envia requisicao
            sendto(sockfd, &req_pkt, sizeof(packet), 0, (const struct sockaddr *)&server_addr, sizeof(server_addr));

            //aguarda resposta
            packet ack_pkt;
            n = recvfrom(sockfd, &ack_pkt, sizeof(packet), 0, NULL, NULL);
            if (n > 0 && ack_pkt.type == TYPE_ACK_REQ) {
                now = time(0);
                t = localtime(&now);
                strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d %H:%M:%S", t);

                // imprime resultado
                printf("%s server %s id req %u dest %s value %u new_balance %u\n", time_buffer, inet_ntoa(server_addr.sin_addr), ack_pkt.seqn, ip_str, valor, ack_pkt.balance);
            } else {
                // timeouts e retransmissao
                fprintf(stderr, "Erro: ACK não recebido ou pacote inválido.\n");
            }
        }
    } else {
        printf("Nenhuma resposta do servidor recebida.\n");
    }
    

    close(sockfd);
    return 0;
}