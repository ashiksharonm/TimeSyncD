/* Exit 0 if GET /healthz on 127.0.0.1:<port> answers 200 (used by the container HEALTHCHECK). */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

int main(int argc, char **argv) {
    int port = argc > 1 ? atoi(argv[1]) : 9100;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct timeval tv = {2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof a) < 0) return 1;
    const char *req = "GET /healthz HTTP/1.0\r\n\r\n";
    if (write(fd, req, strlen(req)) < 0) return 1;
    char buf[64] = {0};
    if (read(fd, buf, sizeof buf - 1) <= 0) return 1;
    return strncmp(buf, "HTTP/1.1 200", 12) == 0 ? 0 : 1;
}
