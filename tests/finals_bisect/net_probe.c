/* tests/finals_bisect: the check Steam failed on a rewritten runtime. Resolves
 * steamconnecttest.com, connects to port 80 and fetches /204 through Winsock. */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <string.h>

int main( void )
{
    WSADATA wsa;
    struct addrinfo hints = {0}, *res;
    char buf[256];
    const char *req = "GET /204 HTTP/1.1\r\nHost: steamconnecttest.com\r\nConnection: close\r\n\r\n";
    SOCKET s;
    int r, n;
    DWORD timeout = 5000;

    WSAStartup( MAKEWORD(2, 2), &wsa );
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if ((r = getaddrinfo( "steamconnecttest.com", "80", &hints, &res )))
    {
        printf( "getaddrinfo FAILED %d\n", r );
        return 1;
    }
    printf( "resolved ok\n" );
    s = socket( res->ai_family, res->ai_socktype, res->ai_protocol );
    setsockopt( s, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout) );
    if (connect( s, res->ai_addr, (int)res->ai_addrlen ))
    {
        printf( "connect FAILED %d\n", WSAGetLastError() );
        return 1;
    }
    send( s, req, (int)strlen( req ), 0 );
    n = recv( s, buf, sizeof(buf) - 1, 0 );
    if (n <= 0)
    {
        printf( "recv FAILED %d\n", WSAGetLastError() );
        return 1;
    }
    buf[n] = 0;
    printf( "reply: %.*s\n", (int)strcspn( buf, "\r\n" ), buf );
    return strstr( buf, " 204" ) ? 0 : 1;
}
