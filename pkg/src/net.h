#pragma once
#include <netinet/in.h>

bool net_init();
// Запрос A-записи у DNS-сервера server:port напрямую (UDP). Запасной путь, если getaddrinfo не работает.
bool net_resolve_dns(const char* host, const char* server, int port, struct in_addr* out, int timeout_ms);
bool net_resolve(const char* host, struct in_addr* out);
int  net_connect(const char* host, int port, int timeout_ms);  // блокирующий, сокет или -1
int  net_connect_nb(const char* ip, int port);                 // неблокирующий, ip только цифрами