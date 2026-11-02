/*
 * What Steam's networking does: a UDP socket with IP_RECVTOS on, a datagram sent to itself
 * with a known TOS, then WSARecvMsg with a control buffer. Windows (and Proton) return an
 * IP_TOS control message carrying the byte; Wine on macOS used to return none, which made
 * Steam assert inside every game process ("No control data returned even though we asked
 * for TOS?") and write a minidump.
 *
 *   x86_64-w64-mingw32-gcc -O1 -Wall tests/windows_probe/recvtos_probe.c \
 *     -o runtime/build/probe/recvtos-probe.exe -lws2_32
 *
 * Exit 0: an IP_TOS control message arrived. 1: none. 2: setup failed.
 */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <stdio.h>

int main(void)
{
    WSADATA wsa;
    SOCKET s;
    struct sockaddr_in addr = { 0 };
    int len = sizeof(addr), on = 1, tos = 0x88, found = 0;
    DWORD got = 0, bytes;
    GUID guid = WSAID_WSARECVMSG;
    LPFN_WSARECVMSG recvmsg_fn = NULL;
    char payload[16] = "tos-probe", buf[64], control[256];
    WSABUF data = { sizeof(buf), buf };
    WSAMSG msg = { 0 };
    WSACMSGHDR *cmsg;

    if (WSAStartup(MAKEWORD(2, 2), &wsa)) return 2;
    s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (s == INVALID_SOCKET || bind(s, (struct sockaddr *)&addr, sizeof(addr))
            || getsockname(s, (struct sockaddr *)&addr, &len)) return 2;
    printf("setsockopt(IP_RECVTOS): %d\n", setsockopt(s, IPPROTO_IP, IP_RECVTOS, (char *)&on, sizeof(on)));
    printf("setsockopt(IP_TOS 0x%x): %d\n", tos, setsockopt(s, IPPROTO_IP, IP_TOS, (char *)&tos, sizeof(tos)));
    if (WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof(guid), &recvmsg_fn, sizeof(recvmsg_fn),
                 &got, NULL, NULL) || !recvmsg_fn) return 2;
    if (sendto(s, payload, sizeof(payload), 0, (struct sockaddr *)&addr, sizeof(addr)) != sizeof(payload)) return 2;

    msg.lpBuffers = &data; msg.dwBufferCount = 1;
    msg.Control.buf = control; msg.Control.len = sizeof(control);
    if (recvmsg_fn(s, &msg, &bytes, NULL, NULL)) { printf("WSARecvMsg failed %d\n", WSAGetLastError()); return 2; }
    printf("received %lu bytes, %lu bytes of control data\n", bytes, msg.Control.len);
    for (cmsg = WSA_CMSG_FIRSTHDR(&msg); cmsg; cmsg = WSA_CMSG_NXTHDR(&msg, cmsg))
    {
        printf("  control message level %d type %d len %lu", cmsg->cmsg_level, cmsg->cmsg_type,
               (unsigned long)cmsg->cmsg_len);
        if (cmsg->cmsg_level == IPPROTO_IP && cmsg->cmsg_type == IP_TOS)
        {
            printf(" -> IP_TOS 0x%x", *(int *)WSA_CMSG_DATA(cmsg));
            found = 1;
        }
        printf("\n");
    }
    printf(found ? "IP_TOS control message present\n" : "no IP_TOS control message\n");
    closesocket(s);
    WSACleanup();
    return found ? 0 : 1;
}
