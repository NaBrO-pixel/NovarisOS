/* winsock.c - a Windows program that opens a TCP connection.
 *
 * Milestone 41 gave Novaris BSD sockets a ring-3 process can call, and
 * that raised a question the documentation had already answered in the
 * negative: whether a *Windows* program could now reach the network.
 *
 * The answer is not obvious either way. Wine's ws2_32 is two halves like
 * every other builtin: the PE side a program links against, and a Unix
 * side that does the work. That Unix side talks to `\Device\Afd`, and
 * Wine's AFD implementation is written on top of ordinary BSD sockets -
 * socket(), connect(), send(), poll(). Those are exactly the calls
 * Milestone 41 added. So the interesting question is whether the path
 * happens to be complete, and the only way to find out is to run one.
 *
 * Deliberately the smallest possible program that answers it: WSAStartup,
 * one socket, one connect, one send, one recv. No name resolution -
 * getaddrinfo pulls in a resolver and would confuse "the socket layer
 * works" with "the resolver works", which are separate questions.
 *
 * Usage: winsock.exe <dotted-quad> <port>
 */

#include <winsock2.h>
#include <windows.h>
#include <stdio.h>

int main(int argc, char** argv) {
    const char* host = argc > 1 ? argv[1] : "10.0.2.2";
    int port = argc > 2 ? atoi(argv[2]) : 80;

    printf("winsock: connecting to %s:%d\n", host, port);
    fflush(stdout);

    WSADATA wsa;
    int r = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (r != 0) {
        printf("[--] WSAStartup failed: %d\n", r);
        return 1;
    }
    printf("[ok] WSAStartup\n");
    fflush(stdout);

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        printf("[--] socket failed: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }
    printf("[ok] socket\n");
    fflush(stdout);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = inet_addr(host);

    if (connect(s, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        printf("[--] connect failed: %d\n", WSAGetLastError());
        closesocket(s);
        WSACleanup();
        return 1;
    }
    printf("[ok] connect\n");
    fflush(stdout);

    /* HTTP/1.0, so the server's close is the framing and there is no
     * keep-alive to shut down. */
    static const char req[] =
        "GET / HTTP/1.0\r\nHost: novaris\r\nConnection: close\r\n\r\n";
    int sent = send(s, req, (int)sizeof(req) - 1, 0);
    if (sent != (int)sizeof(req) - 1) {
        printf("[--] send returned %d (error %d)\n", sent, WSAGetLastError());
        closesocket(s);
        WSACleanup();
        return 1;
    }
    printf("[ok] sent %d bytes\n", sent);
    fflush(stdout);

    char buf[2048];
    int total = 0, shown = 0;
    for (;;) {
        int n = recv(s, buf, (int)sizeof(buf) - 1, 0);
        if (n <= 0) break;
        if (!shown) {
            /* The status line only, so the transcript is an assertion
             * rather than a web page. */
            buf[n] = '\0';
            char* nl = strpbrk(buf, "\r\n");
            if (nl) *nl = '\0';
            printf("[ok] %s\n", buf);
            fflush(stdout);
            shown = 1;
        }
        total += n;
    }

    printf("[ok] read %d bytes to end of stream\n", total);
    closesocket(s);
    WSACleanup();
    printf("[ok] closed\n");
    printf("winsock: a Windows program reached the network\n");
    fflush(stdout);
    return 0;
}
