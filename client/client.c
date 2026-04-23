#include "../include/protocol.h"
#include "../include/utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <signal.h>

int g_client_running = 1;

void handle_sigint(int sig) {
    (void)sig;
    g_client_running = 0;
}

int main(int argc, char* argv[]) {
    int mode_tcp = 1;
    char server_ip[64] = "127.0.0.1";
    int port = 9000;
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            mode_tcp = (strcmp(argv[++i], "tcp") == 0);
        } else if (strcmp(argv[i], "--server-ip") == 0 && i + 1 < argc) {
            strncpy(server_ip, argv[++i], sizeof(server_ip) - 1);
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        }
    }

    signal(SIGINT, handle_sigint);
    signal(SIGPIPE, SIG_IGN);

    int sock = socket(AF_INET, mode_tcp ? SOCK_STREAM : SOCK_DGRAM, 0);
    if (sock < 0) {
        LOG_ERR("Failed to create socket");
        return 1;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    inet_pton(AF_INET, server_ip, &server_addr.sin_addr);

    if (mode_tcp) {
        if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
            LOG_ERR("Failed to connect to server %s:%d", server_ip, port);
            close(sock);
            return 1;
        }
        LOG_INFO("Connected to server %s:%d via TCP", server_ip, port);
    } else {
        LOG_INFO("Using UDP to server %s:%d", server_ip, port);
        // Register with server
        sync_message_t reg_msg;
        memset(&reg_msg, 0, sizeof(reg_msg));
        reg_msg.type = MSG_REGISTER;
        sendto(sock, &reg_msg, sizeof(reg_msg), 0, (struct sockaddr*)&server_addr, sizeof(server_addr));
        LOG_INFO("Sent MSG_REGISTER");
    }

    // Simulated local drift (e.g. random static offset up to 100ms)
    srand((unsigned int)(get_time_ns() ^ getpid()));
    int64_t drift_offset = (rand() % 100000) * 1000LL; // max 100ms drift in ns

    struct timeval tv;
    tv.tv_sec = 2; // Setup receive timeout
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

    while (g_client_running) {
        sync_message_t msg;
        int bytes;
        if (mode_tcp) {
            bytes = recv(sock, &msg, sizeof(msg), 0);
            if (bytes == 0) {
                LOG_INFO("Server closed connection");
                break;
            }
        } else {
            socklen_t addrlen = sizeof(server_addr);
            bytes = recvfrom(sock, &msg, sizeof(msg), 0, (struct sockaddr*)&server_addr, &addrlen);
        }

        if (bytes > 0 && msg.type == MSG_SYNC) {
            uint64_t receive_time = get_time_ns();
            
            // Artificial drift addition to simulated local clock
            uint64_t local_clock = receive_time + drift_offset;
            int64_t drift = (int64_t)local_clock - (int64_t)msg.server_ts_ns;
            
            uint64_t one_way_latency_estimate = (receive_time - msg.server_ts_ns);
            
            LOG_INFO("SYNC received. Latency est: %lluus, Offset (Drift): %lldus", 
                     (unsigned long long)one_way_latency_estimate / 1000, 
                     (long long)drift / 1000);
            
            // Send response back
            sync_message_t resp;
            memset(&resp, 0, sizeof(resp));
            resp.type = MSG_RESPONSE;
            resp.server_ts_ns = msg.server_ts_ns; // bounce back original ts for RTT calc
            resp.client_ts_ns = receive_time;     // exact receive time
            
            if (mode_tcp) {
                send(sock, &resp, sizeof(resp), 0);
            } else {
                sendto(sock, &resp, sizeof(resp), 0, (struct sockaddr*)&server_addr, sizeof(server_addr));
            }
        }
    }

    close(sock);
    LOG_INFO("Client shut down.");
    return 0;
}
