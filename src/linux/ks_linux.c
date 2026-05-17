/**
 * @file ks_linux.c
 * @brief KernelSocket — Идеальная и безопасная реализация для ядра Linux.
 */

#include "ks_linux_internal.h"
#include <linux/inet.h>   /* Для in4_pton */
#include <linux/slab.h>   /* Для kmalloc/kfree */

/* =========================================================================
 * Вспомогательные функции для работы с IP
 * ========================================================================= */
static int ks_str_to_sockaddr(const char *ip, unsigned short port, struct sockaddr_in *addr) {
    memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    addr->sin_port = htons(port);
    
    /* Конвертируем строку ("192.168.1.1") в бинарный адрес ядра */
    if (!in4_pton(ip, -1, (u8 *)&addr->sin_addr.s_addr, '\0', NULL)) {
        pr_err("[KernelSocket] Ошибка: Неверный IP адрес '%s'\n", ip);
        return -1;
    }
    return 0;
}

static void ks_sockaddr_to_str(struct sockaddr_in *addr, char *ip, unsigned short *port) {
    if (ip) {
        /* Формат %pI4 - это ядерный аналог inet_ntoa */
        snprintf(ip, 16, "%pI4", &addr->sin_addr.s_addr);
    }
    if (port) {
        *port = ntohs(addr->sin_port);
    }
}

/* =========================================================================
 * Реализация публичного API
 * ========================================================================= */

KS_SOCKET* ksSocket(int Protocol) {
    KS_SOCKET* ks;
    int type = (Protocol == KS_TCP) ? SOCK_STREAM : SOCK_DGRAM;
    int proto = (Protocol == KS_TCP) ? IPPROTO_TCP : IPPROTO_UDP;

    ks = kmalloc(sizeof(KS_SOCKET), GFP_KERNEL);
    if (!ks) return NULL;

    /* Создаем сокет пространства ядра */
    if (sock_create_kern(&init_net, AF_INET, type, proto, &ks->sock) < 0) {
        kfree(ks);
        return NULL;
    }

    ks->Protocol = Protocol;
    return ks;
}

int ksBind(KS_SOCKET* Sock, const char* Ip, unsigned short Port) {
    struct sockaddr_in addr;
    
    if (!Sock || !Ip) return KS_ERR_INVAL;
    if (ks_str_to_sockaddr(Ip, Port, &addr) < 0) return KS_ERR_INVAL;

    if (kernel_bind(Sock->sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        return KS_ERR;
    }
    return KS_OK;
}

int ksListen(KS_SOCKET* Sock, int Backlog) {
    if (!Sock || Sock->Protocol != KS_TCP) return KS_ERR_INVAL;
    
    if (kernel_listen(Sock->sock, Backlog) < 0) {
        return KS_ERR;
    }
    return KS_OK;
}

int ksAccept(KS_SOCKET* Sock, KS_SOCKET** NewSock, char* ClientIp, unsigned short* ClientPort) {
    struct socket *new_sock = NULL;
    struct sockaddr_in addr;
    int err;

    if (!Sock || !NewSock) return KS_ERR_INVAL;

    /* 0 означает, что вызов блокирующий (ждем клиента вечно) */
    err = kernel_accept(Sock->sock, &new_sock, 0);
    if (err < 0) return KS_ERR;

    *NewSock = kmalloc(sizeof(KS_SOCKET), GFP_KERNEL);
    if (!*NewSock) {
        sock_release(new_sock);
        return KS_ERR_NOMEM;
    }

    (*NewSock)->Protocol = KS_TCP;
    (*NewSock)->sock = new_sock;

    /* Достаем IP и порт подключившегося клиента (2 означает Peer/Удаленный узел) */
    if (ClientIp || ClientPort) {
        err = new_sock->ops->getname(new_sock, (struct sockaddr *)&addr, 2);
        if (err >= 0) {
            ks_sockaddr_to_str(&addr, ClientIp, ClientPort);
        }
    }

    return KS_OK;
}

int ksConnect(KS_SOCKET* Sock, const char* Ip, unsigned short Port) {
    struct sockaddr_in addr;
    int err;

    if (!Sock || !Ip) return KS_ERR_INVAL;
    if (ks_str_to_sockaddr(Ip, Port, &addr) < 0) return KS_ERR_INVAL;

    err = kernel_connect(Sock->sock, (struct sockaddr *)&addr, sizeof(addr), 0);
    if (err == -ECONNREFUSED) return KS_ERR_CONN;
    if (err < 0) return KS_ERR;

    return KS_OK;
}

int ksSend(KS_SOCKET* Sock, const void* Buf, unsigned int Len) {
    struct msghdr msg;
    struct kvec vec;

    if (!Sock || !Buf || Len == 0) return KS_ERR_INVAL;

    /* КРИТИЧЕСКИ ВАЖНО: обнуляем msghdr, иначе ядро упадет! */
    memset(&msg, 0, sizeof(msg));
    
    vec.iov_base = (void *)Buf;
    vec.iov_len = Len;

    /* 1 - это количество элементов в массиве iovec (у нас один буфер) */
    return kernel_sendmsg(Sock->sock, &msg, &vec, 1, Len);
}

int ksRecv(KS_SOCKET* Sock, void* Buf, unsigned int Len) {
    struct msghdr msg;
    struct kvec vec;
    int ret;

    if (!Sock || !Buf || Len == 0) return KS_ERR_INVAL;

    memset(&msg, 0, sizeof(msg));
    vec.iov_base = Buf;
    vec.iov_len = Len;

    /* 0 в конце - вызов блокирующий */
    ret = kernel_recvmsg(Sock->sock, &msg, &vec, 1, Len, 0);
    
    if (ret < 0) return KS_ERR;
    return ret; /* Возвращаем количество прочитанных байт */
}

int ksSendTo(KS_SOCKET* Sock, const void* Buf, unsigned int Len, const char* Ip, unsigned short Port) {
    struct msghdr msg;
    struct kvec vec;
    struct sockaddr_in addr;
    int ret;

    if (!Sock || !Buf || Len == 0 || !Ip) return KS_ERR_INVAL;
    if (ks_str_to_sockaddr(Ip, Port, &addr) < 0) return KS_ERR_INVAL;

    memset(&msg, 0, sizeof(msg));
    
    /* Для UDP явно указываем адрес получателя в самой структуре сообщения */
    msg.msg_name = &addr;
    msg.msg_namelen = sizeof(addr);

    vec.iov_base = (void *)Buf;
    vec.iov_len = Len;

    ret = kernel_sendmsg(Sock->sock, &msg, &vec, 1, Len);
    return (ret < 0) ? KS_ERR : ret;
}

int ksRecvFrom(KS_SOCKET* Sock, void* Buf, unsigned int Len, char* SrcIp, unsigned short* SrcPort) {
    struct msghdr msg;
    struct kvec vec;
    struct sockaddr_in addr;
    int ret;

    if (!Sock || !Buf || Len == 0) return KS_ERR_INVAL;

    memset(&msg, 0, sizeof(msg));
    memset(&addr, 0, sizeof(addr));
    
    /* Подготавливаем поля для записи адреса отправителя ядром */
    msg.msg_name = &addr;
    msg.msg_namelen = sizeof(addr);

    vec.iov_base = Buf;
    vec.iov_len = Len;

    ret = kernel_recvmsg(Sock->sock, &msg, &vec, 1, Len, 0);
    if (ret < 0) return KS_ERR;

    ks_sockaddr_to_str(&addr, SrcIp, SrcPort);
    return ret;
}

void ksClose(KS_SOCKET* Sock) {
    if (!Sock) return;
    
    if (Sock->sock) {
        sock_release(Sock->sock);
    }
    kfree(Sock);
}