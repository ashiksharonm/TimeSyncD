#include "../include/server.h"
#include "../include/utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <fcntl.h>
#include <errno.h>

#define MAX_CLIENTS 1024

typedef struct {
    int fd;                   // TCP fd
    struct sockaddr_in addr;  // UDP addr
    int active;
} client_node_t;

client_node_t g_clients[MAX_CLIENTS];
pthread_mutex_t g_clients_mutex = PTHREAD_MUTEX_INITIALIZER;
int g_server_sock = -1;
extern int g_server_running;
extern server_config_t g_config;

static void set_nonblocking(int sock) {
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
}

int network_init(const server_config_t* config) {
    memset(g_clients, 0, sizeof(g_clients));
    
    int type = config->mode_tcp ? SOCK_STREAM : SOCK_DGRAM;
    g_server_sock = socket(AF_INET, type, 0);
    if (g_server_sock < 0) {
        LOG_ERR("Failed to create socket");
        return -1;
    }
    
    int opt = 1;
    setsockopt(g_server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(config->port);
    
    if (bind(g_server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        LOG_ERR("Bind failed");
        return -1;
    }
    
    if (config->mode_tcp && listen(g_server_sock, 128) < 0) {
        LOG_ERR("Listen failed");
        return -1;
    }
    
    set_nonblocking(g_server_sock);
    return 0;
}

void network_cleanup(void) {
    if (g_server_sock >= 0) close(g_server_sock);
    pthread_mutex_lock(&g_clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].active && g_config.mode_tcp) {
            close(g_clients[i].fd);
        }
    }
    pthread_mutex_unlock(&g_clients_mutex);
}

static void update_active_client_count() {
    uint32_t active = 0;
    pthread_mutex_lock(&g_clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (g_clients[i].active) active++;
    }
    pthread_mutex_unlock(&g_clients_mutex);
    sync_engine_set_clients(active);
}

void* accept_thread_func(void* arg) {
    (void)arg;
    if (!g_config.mode_tcp) return NULL;
    
    LOG_INFO("Accept thread started for TCP.");
    
    struct timeval tv;
    fd_set readfds;
    
    while (g_server_running) {
        FD_ZERO(&readfds);
        FD_SET(g_server_sock, &readfds);
        tv.tv_sec = 0;
        tv.tv_usec = 100000; // 100ms
        
        int ret = select(g_server_sock + 1, &readfds, NULL, NULL, &tv);
        if (ret > 0 && FD_ISSET(g_server_sock, &readfds)) {
            struct sockaddr_in client_addr;
            socklen_t addr_len = sizeof(client_addr);
            int client_sock = accept(g_server_sock, (struct sockaddr*)&client_addr, &addr_len);
            
            if (client_sock >= 0) {
                set_nonblocking(client_sock);
                pthread_mutex_lock(&g_clients_mutex);
                int added = 0;
                for (int i = 0; i < MAX_CLIENTS; i++) {
                    if (!g_clients[i].active) {
                        g_clients[i].fd = client_sock;
                        g_clients[i].active = 1;
                        added = 1;
                        break;
                    }
                }
                
                if (!added) {
                    LOG_ERR("Max clients reached. Rejecting connection.");
                    close(client_sock);
                } else {
                    LOG_INFO("New TCP client connected from %s:%d.", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
                }
                pthread_mutex_unlock(&g_clients_mutex);
                update_active_client_count();
            }
        }
    }
    return NULL;
}

static void handle_client_message(int index, sync_message_t* msg, struct sockaddr_in* src_addr) {
    (void)index;
    if (msg->type == MSG_REGISTER && !g_config.mode_tcp && src_addr) {
        pthread_mutex_lock(&g_clients_mutex);
        int added = 0;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (g_clients[i].active && 
                g_clients[i].addr.sin_addr.s_addr == src_addr->sin_addr.s_addr &&
                g_clients[i].addr.sin_port == src_addr->sin_port) {
                added = 1; 
                break;
            }
        }
        
        if (!added) {
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (!g_clients[i].active) {
                    g_clients[i].addr = *src_addr;
                    g_clients[i].active = 1;
                    added = 1;
                    LOG_INFO("New UDP client registered from %s:%d", inet_ntoa(src_addr->sin_addr), ntohs(src_addr->sin_port));
                    break;
                }
            }
        }
        pthread_mutex_unlock(&g_clients_mutex);
        if (added) {
            update_active_client_count();
        }
    } else if (msg->type == MSG_RESPONSE) {
        uint64_t now = get_time_ns();
        // Server derives round-trip latency. True one-way latency approximation = (RTT / 2)
        uint64_t latency = (now - msg->server_ts_ns) / 2;
        sync_engine_update_metrics(latency);
    }
}

void* worker_thread_func(void* arg) {
    (void)arg;
    LOG_INFO("Worker thread started...");
    
    struct timeval tv;
    fd_set readfds;
    
    while (g_server_running) {
        FD_ZERO(&readfds);
        int max_fd = -1;
        
        if (g_config.mode_tcp) {
            pthread_mutex_lock(&g_clients_mutex);
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (g_clients[i].active) {
                    FD_SET(g_clients[i].fd, &readfds);
                    if (g_clients[i].fd > max_fd) max_fd = g_clients[i].fd;
                }
            }
            pthread_mutex_unlock(&g_clients_mutex);
        } else {
            FD_SET(g_server_sock, &readfds);
            max_fd = g_server_sock;
        }
        
        if (max_fd == -1) {
            usleep(100000); // Wait dynamically if no clients yet
            continue;
        }
        
        tv.tv_sec = 0;
        tv.tv_usec = 100000;
        int ret = select(max_fd + 1, &readfds, NULL, NULL, &tv);
        
        if (ret > 0) {
            if (g_config.mode_tcp) {
                pthread_mutex_lock(&g_clients_mutex);
                for (int i = 0; i < MAX_CLIENTS; i++) {
                    if (g_clients[i].active && FD_ISSET(g_clients[i].fd, &readfds)) {
                        sync_message_t msg;
                        int bytes = recv(g_clients[i].fd, &msg, sizeof(msg), 0);
                        if (bytes == sizeof(msg)) {
                            handle_client_message(i, &msg, NULL);
                        } else if (bytes <= 0) {
                            LOG_INFO("TCP Client disconnected.");
                            close(g_clients[i].fd);
                            g_clients[i].active = 0;
                        }
                    }
                }
                pthread_mutex_unlock(&g_clients_mutex);
                update_active_client_count(); 
            } else {
                if (FD_ISSET(g_server_sock, &readfds)) {
                    sync_message_t msg;
                    struct sockaddr_in src_addr;
                    socklen_t addrlen = sizeof(src_addr);
                    int bytes = recvfrom(g_server_sock, &msg, sizeof(msg), 0, (struct sockaddr*)&src_addr, &addrlen);
                    if (bytes == sizeof(msg)) {
                        handle_client_message(-1, &msg, &src_addr);
                    }
                }
            }
        }
    }
    return NULL;
}

void network_broadcast_sync(uint64_t server_ts_ns) {
    sync_message_t msg;
    msg.type = MSG_SYNC;
    msg.client_id = 0;
    msg.server_ts_ns = server_ts_ns;
    msg.client_ts_ns = 0;
    
    pthread_mutex_lock(&g_clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].active) {
            if (g_config.mode_tcp) {
                send(g_clients[i].fd, &msg, sizeof(msg), 0);
            } else {
                sendto(g_server_sock, &msg, sizeof(msg), 0, (struct sockaddr*)&g_clients[i].addr, sizeof(g_clients[i].addr));
            }
        }
    }
    pthread_mutex_unlock(&g_clients_mutex);
}
