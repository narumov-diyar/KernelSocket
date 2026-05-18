/**
 * @file ks_linux.c
 * @brief Реализация KernelSocket для ядра Linux.
 *
 * @details
 * Файл предоставляет Linux-backend для публичного API `ks_api.h` и использует
 * стандартные kernel socket примитивы (`sock_create_kern`, `kernel_sendmsg`,
 * `kernel_recvmsg` и др.). Реализация ориентирована на безопасный код с
 * предотвращением Kernel Panic за счет строгой инициализации структур.
 */

#include "ks_linux_internal.h"
#include <linux/inet.h>   /* Для in4_pton */
#include <linux/slab.h>   /* Для kmalloc/kfree */

/* =========================================================================
 * Вспомогательные функции для работы с IP
 * ========================================================================= */

/**
 * @brief Преобразует строковый IPv4 и порт в `sockaddr_in`.
 *
 * @param[in]  ip   IPv4-адрес в формате строки (например, "127.0.0.1").
 * @param[in]  port Порт в host byte order.
 * @param[out] addr Буфер для записи бинарного адреса.
 * 
 * @return `0` при успехе, `-1` при ошибке формата адреса.
 */
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

/**
 * @brief Преобразует бинарный `sockaddr_in` в строку IPv4 и порт.
 *
 * @param[in]  addr Адрес источника.
 * @param[out] ip   Буфер под строку IPv4 (может быть `NULL`).
 * @param[out] port Буфер под порт (может быть `NULL`).
 */
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

/**
 * @brief Реализация функции ksSocket для платформы Linux.
 * @details Выделяет память под структуру `KS_SOCKET` с флагом `GFP_KERNEL`
 * и создает ядерный сокет с помощью `sock_create_kern`.
 */
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

/**
 * @brief Реализация функции ksBind для платформы Linux.
 * @details Выполняет привязку сокета с использованием `kernel_bind`.
 */
int ksBind(KS_SOCKET* Sock, const char* Ip, unsigned short Port) {
	struct sockaddr_in addr;
	
	if (!Sock || !Ip) return KS_ERR_INVAL;
	if (ks_str_to_sockaddr(Ip, Port, &addr) < 0) return KS_ERR_INVAL;

	if (kernel_bind(Sock->sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		return KS_ERR;
	}
	return KS_OK;
}

/**
 * @brief Реализация функции ksListen для платформы Linux.
 * @details Переводит сокет в режим прослушивания с помощью `kernel_listen`.
 */
int ksListen(KS_SOCKET* Sock, int Backlog) {
	if (!Sock || Sock->Protocol != KS_TCP) return KS_ERR_INVAL;
	
	if (kernel_listen(Sock->sock, Backlog) < 0) {
		return KS_ERR;
	}
	return KS_OK;
}

/**
 * @brief Реализация функции ksAccept для платформы Linux.
 * @details Использует `kernel_accept` в блокирующем режиме. После принятия
 * соединения извлекает адрес клиента через функцию `ops->getname`.
 */
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

/**
 * @brief Реализация функции ksConnect для платформы Linux.
 * @details Устанавливает соединение с удаленным узлом через `kernel_connect`.
 */
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

/**
 * @brief Реализация функции ksSend для платформы Linux.
 * 
 * @warning Kernel Panic Риск: Перед вызовом `kernel_sendmsg` структура `msghdr` 
 * ОБЯЗАТЕЛЬНО должна быть обнулена (`memset`). В противном случае использование
 * мусорных указателей внутри нее приведет к падению ядра.
 */
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

/**
 * @brief Реализация функции ksRecv для платформы Linux.
 * @details Читает данные блокирующим вызовом `kernel_recvmsg`. 
 * Структура `msghdr` предварительно обнуляется для безопасности.
 */
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

/**
 * @brief Реализация функции ksSendTo для платформы Linux.
 * @details Заполняет поле `msg_name` в структуре `msghdr` адресом назначения
 * и отправляет датаграмму через `kernel_sendmsg`.
 */
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

/**
 * @brief Реализация функции ksRecvFrom для платформы Linux.
 * @details Подготавливает буфер `msg_name` для получения адреса отправителя
 * ядром во время вызова `kernel_recvmsg`.
 */
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

/**
 * @brief Реализация функции ksClose для платформы Linux.
 * @details Освобождает ядерный сокет функцией `sock_release` и память `kfree`.
 */
void ksClose(KS_SOCKET* Sock) {
	if (!Sock) return;
	
	if (Sock->sock) {
		sock_release(Sock->sock);
	}
	kfree(Sock);
}