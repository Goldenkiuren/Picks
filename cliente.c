#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <sys/select.h>
#include <stdbool.h>
#include <pthread.h>
#include "common.h"

//constantes globais
#define BROADCAST_IP "255.255.255.255"
#define MAX_RETRIES 3
#define TIMEOUT_MS 500
#define MSG_BUFFER_SIZE 512

//globais do cliente
//requisição
char req_ip[20];
uint32_t req_valor;
bool req_ready = false;
pthread_mutex_t req_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t req_cond = PTHREAD_COND_INITIALIZER;
//resposta
char resp_msg[MSG_BUFFER_SIZE];
bool resp_ready = false;
pthread_mutex_t resp_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t resp_cond = PTHREAD_COND_INITIALIZER;
//flags de controle
bool program_exit = false;
bool server_found = false;

/*
função produtora para a thread de output
ela adquire o mutex de resposta, quando livre copia a mensagem para o buffer global
e sinaliza para a thread de output que tem dados prontos
*/
void send_to_output(const char* msg) {
    pthread_mutex_lock(&resp_mutex);
    while (resp_ready) {
        pthread_cond_wait(&resp_cond, &resp_mutex);
    }
    //produz mensagem
    strncpy(resp_msg, msg, MSG_BUFFER_SIZE - 1);
    resp_msg[MSG_BUFFER_SIZE - 1] = '\0';
    resp_ready = true;
    pthread_cond_signal(&resp_cond);
    pthread_mutex_unlock(&resp_mutex);
}

/*
obtém a data/hora atual e a formata em uma string "YYYY-MM-DD HH:MM:SS".
*/
void get_current_time_str(char* buffer, size_t buffer_size) {
    time_t now = time(0);
    struct tm *t = localtime(&now);
    strftime(buffer, buffer_size, "%Y-%m-%d %H:%M:%S", t);
}

/*
thread consumidora para a stdout
fica bloqueada aguardando resp_cond
quuando true, imprime a mensagem global 'resp_msg' e sinalize que buffer esta livre
*/
void* output_thread_func(void* arg) {
    pthread_mutex_lock(&resp_mutex);
    while (true) {
        //espera resposta ou fim do programa
        while (!resp_ready && !program_exit) {
            pthread_cond_wait(&resp_cond, &resp_mutex);
        }
        
        if (program_exit && !resp_ready) {
            break;
        }

        printf("%s\n", resp_msg);
        resp_ready = false;
        pthread_cond_signal(&resp_cond); 
    }
    pthread_mutex_unlock(&resp_mutex);
    return NULL;
}

/*
thread "produtora" para stdin
espera até que o servidor seja descoberto e então entra em um loop lendo IP e valor
quando a entrada é válida, trava req_mutex, espera se o bufferd e requisição não estiver livre
depois preenche 'req_ip' e 'req_valor' e e sinaliza 'req_cond' para acordar a thread principal
em caso de EOF é definido 'program_exit'
*/
void* input_thread_func(void* arg) {
    char ip_str[20];
    uint32_t valor;
    
    // espera ate a thread main encontrar o servidor pela primeira vez
    while (!server_found) {
        bool exit_flag;
        pthread_mutex_lock(&resp_mutex);
        exit_flag = program_exit;
        pthread_mutex_unlock(&resp_mutex);
        if (exit_flag) return NULL;
        usleep(100000); 
    }

    // loop de leitura da entrada
    while (scanf("%s %u", ip_str, &valor) == 2) {
        struct in_addr temp_addr;
        if (inet_aton(ip_str, &temp_addr) == 0) {
            send_to_output("Erro: IP inválido. Tente novamente.");
            continue;
        }
        pthread_mutex_lock(&req_mutex);
        // espera main processar a req anterior
        while (req_ready) {
            pthread_cond_wait(&req_cond, &req_mutex);
        }
        
        strcpy(req_ip, ip_str);
        req_valor = valor;
        req_ready = true;
        pthread_cond_signal(&req_cond);
        pthread_mutex_unlock(&req_mutex);
    }
    
    send_to_output("Fim de entrada (Ctrl+D) detectado. Encerrando...");
    pthread_mutex_lock(&resp_mutex);
    program_exit = true;
    pthread_mutex_unlock(&resp_mutex);
    
    pthread_cond_signal(&req_cond);
    pthread_cond_signal(&resp_cond);
    
    return NULL;
}

int main(int argc, char *argv[]) {
    
    if (argc != 2) {
        fprintf(stderr, "Use: ./cliente <porta>\n");
        return 1;
    }

    int port = atoi(argv[1]);
    int sockfd;
    struct sockaddr_in server_addr, broadcast_addr;
    packet discovery_pkt, response_pkt;

    // inicializa a thread de output imediatamente para logar tudo
    pthread_t output_tid;
    if (pthread_create(&output_tid, NULL, output_thread_func, NULL) != 0) {
        perror("falha ao criar thread de output");
        exit(EXIT_FAILURE);
    }

    //criando socket UDP
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("falha na criação do socket.");
        pthread_mutex_lock(&resp_mutex);
        program_exit = true; 
        pthread_mutex_unlock(&resp_mutex);
        pthread_cond_signal(&resp_cond);
        exit(EXIT_FAILURE);
    }

    // habilita broadcast no socket
    int broadcast_enable = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_BROADCAST, &broadcast_enable, sizeof(broadcast_enable)) < 0) {
        perror("falha ao habilitar broadcast");
        close(sockfd);
        program_exit = true;
        pthread_cond_signal(&resp_cond);
        exit(EXIT_FAILURE);
    }

    // configura endereco de broadcast
    memset(&broadcast_addr, 0, sizeof(broadcast_addr));
    broadcast_addr.sin_family = AF_INET;
    broadcast_addr.sin_port = htons(port);

    if (inet_aton(BROADCAST_IP, &broadcast_addr.sin_addr) == 0) {
        fprintf(stderr, "Endereco IP invalido\n");
        program_exit = true;
        pthread_cond_signal(&resp_cond);
        exit(EXIT_FAILURE);
    }

    // loop de reconexão
    uint32_t seqn_local = 0; // contador de seq local
    pthread_t input_tid;
    bool input_thread_started = false;

    while(!program_exit) {

        // FASE DE DESCOBERTA
        send_to_output("--- Iniciando descoberta de Servidor (Lider) ---");
        
        server_found = false;
        bool discovery_success = false;

        while (!discovery_success && !program_exit) {
            memset(&discovery_pkt, 0, sizeof(packet));
            discovery_pkt.type = htons(TYPE_DESCOBERTA);
            
            sendto(sockfd, &discovery_pkt, sizeof(packet), 0, (const struct sockaddr *)&broadcast_addr, sizeof(broadcast_addr));

            // pequeno select para esperar resposta
            struct timeval timeout;
            timeout.tv_sec = 1; 
            timeout.tv_usec = 0;
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(sockfd, &readfds);

            int ready = select(sockfd + 1, &readfds, NULL, NULL, &timeout);
            if (ready > 0) {
                socklen_t len = sizeof(server_addr);
                int n = recvfrom(sockfd, &response_pkt, sizeof(packet), 0, (struct sockaddr *)&server_addr, &len);
                if (n > 0 && ntohs(response_pkt.type) == TYPE_ACK_DESCOBERTA) {
                    discovery_success = true;
                }
            } else {
                 send_to_output("Tentando localizar servidor...");
            }
        }

        if (program_exit) break;

        // servidor encontrado
        char time_buffer[100];
        char msg_buffer[MSG_BUFFER_SIZE];
        get_current_time_str(time_buffer, sizeof(time_buffer));
        snprintf(msg_buffer, sizeof(msg_buffer), "%s Lider encontrado: %s", time_buffer, inet_ntoa(server_addr.sin_addr));
        send_to_output(msg_buffer);
        
        server_found = true;

        if (!input_thread_started) {
            if (pthread_create(&input_tid, NULL, input_thread_func, NULL) != 0) {
                perror("falha ao criar thread de input");
                program_exit = true;
                break;
            }
            input_thread_started = true;
        }

        // loop de requisição
        while (true) {
            char local_ip[20];
            uint32_t local_valor;
            uint32_t local_seqn;
            bool exit_flag = false;

            // espera por uma requisição da thread de input
            pthread_mutex_lock(&req_mutex);
            while (!req_ready) {
                pthread_mutex_lock(&resp_mutex);
                exit_flag = program_exit;
                pthread_mutex_unlock(&resp_mutex);
                if (exit_flag) {
                    pthread_mutex_unlock(&req_mutex);
                    goto main_loop_exit; 
                }
                pthread_cond_wait(&req_cond, &req_mutex);
            }

            // checa novamente após acordar
            pthread_mutex_lock(&resp_mutex);
            exit_flag = program_exit;
            pthread_mutex_unlock(&resp_mutex);
            if (exit_flag) {
                pthread_mutex_unlock(&req_mutex);
                goto main_loop_exit; 
            }

            strcpy(local_ip, req_ip);
            local_valor = req_valor;
            
            // incrementa se for uma nova req
            seqn_local++; 
            local_seqn = seqn_local;
            req_ready = false;
            
            pthread_cond_signal(&req_cond); 
            pthread_mutex_unlock(&req_mutex);

            // processa a requisição
            packet req_pkt;
            memset(&req_pkt, 0, sizeof(packet));
            req_pkt.type = htons(TYPE_REQ);
            req_pkt.seqn = htonl(local_seqn);
            req_pkt.value = htonl(local_valor);
            inet_aton(local_ip, &req_pkt.dest_addr);  

            char temp_msg[MSG_BUFFER_SIZE];
            bool ack_received = false;
            bool server_failed = false; // detectar falha total

            for (int retries = 0; retries < MAX_RETRIES; retries++) {
                if (retries > 0) {
                    snprintf(temp_msg, sizeof(temp_msg), "Reenviando req #%u (tentativa %d/%d)...", local_seqn, retries + 1, MAX_RETRIES);
                } else {
                    snprintf(temp_msg, sizeof(temp_msg), "Enviando req #%u para %s (valor: %u)...", local_seqn, local_ip, local_valor);
                }
                send_to_output(temp_msg);
                sendto(sockfd, &req_pkt, sizeof(packet), 0, (const struct sockaddr *)&server_addr, sizeof(server_addr));

                packet ack_pkt;
                struct timeval timeout;
                timeout.tv_sec = 0;
                timeout.tv_usec = TIMEOUT_MS * 1000; 
                fd_set readfds;
                FD_ZERO(&readfds);
                FD_SET(sockfd, &readfds);

                int ready = select(sockfd + 1, &readfds, NULL, NULL, &timeout);
                
                if (ready > 0) { 
                    struct sockaddr_in sender_addr;
                    socklen_t sender_len = sizeof(sender_addr);
                    int n = recvfrom(sockfd, &ack_pkt, sizeof(packet), 0, (struct sockaddr *)&sender_addr, &sender_len);
                    
                    if (n > 0 && (sender_addr.sin_addr.s_addr != server_addr.sin_addr.s_addr ||
                                sender_addr.sin_port != server_addr.sin_port))
                    {
                        continue; 
                    }

                    if (n > 0 && ntohs(ack_pkt.type) == TYPE_ACK_REQ && ntohl(ack_pkt.seqn) == local_seqn) {
                        get_current_time_str(time_buffer, sizeof(time_buffer));
                        snprintf(temp_msg, sizeof(temp_msg), "%s server %s id req %u dest %s value %u new_balance %u", 
                                time_buffer, inet_ntoa(server_addr.sin_addr), local_seqn, local_ip, local_valor, ntohl(ack_pkt.balance));
                        send_to_output(temp_msg);
                        ack_received = true;
                        break;
                    } else if (n > 0 && ntohs(ack_pkt.type) == TYPE_ERROR_REQ) {
                        snprintf(temp_msg, sizeof(temp_msg), "Erro no servidor.");
                        send_to_output(temp_msg);
                        ack_received = true;
                        break;
                    } 
                } else if (ready == 0) {
                    snprintf(temp_msg, sizeof(temp_msg), "Timeout...");
                    send_to_output(temp_msg);
                } 
            } 
            
            if (!ack_received) {
                // servidor caiu
                snprintf(temp_msg, sizeof(temp_msg), "Falha critica: Servidor não responde. Iniciando redescoberta...");
                send_to_output(temp_msg);
                server_failed = true;
            }

            if (server_failed) {
                break; // sai do loop de requisições e volta para o loop de descoberta
            }
        } 
    }

    main_loop_exit:; 

    if (input_thread_started) pthread_join(input_tid, NULL);
    pthread_join(output_tid, NULL);

    
    close(sockfd);
    return 0;
}