/**
 * @file ks_api.h
 * @brief KernelSocket — Единый API для работы с TCP/UDP в режиме ядра (Windows/Linux).
 *
 * @details Этот заголовочный файл предоставляет кроссплатформенный интерфейс для 
 * организации сетевого взаимодействия из драйверов. Поддерживает как клиентские, 
 * так и серверные сценарии (K<->U, U<->K, K<->K). Эмулирует блокирующее поведение 
 * стандартных POSIX-сокетов.
 *
 * @note Проект выполняется командой Kernel Crew в рамках университетской работы.
 */

#ifndef KS_API_H
#define KS_API_H

/* =========================================================================
 * 1. Определение платформы и системные зависимости
 * ========================================================================= */
#if defined(_WIN32) || defined(_WIN64)
  #define KS_PLATFORM_WINDOWS
  #include <ntddk.h>
#elif defined(__linux__) || defined(__KERNEL__)
  #define KS_PLATFORM_LINUX
  #include <linux/kernel.h>
  #include <linux/module.h>
  #include <linux/types.h>
#else
  #error "[KernelSocket] Unsupported platform! Must be compiled for Windows or Linux kernel."
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * 2. Константы (SCREAMING_SNAKE_CASE)
 * ========================================================================= */

/** @name Идентификаторы протоколов */
///@{
#define KS_TCP 0  /**< TCP (потоковый протокол с установкой соединения). */
#define KS_UDP 1  /**< UDP (датаграммный протокол без установления соединения). */
///@}

/** @name Коды возврата */
///@{
#define KS_OK           0  /**< Успешное выполнение. */
#define KS_ERR         -1  /**< Общая ошибка платформы. */
#define KS_ERR_INVAL   -2  /**< Переданы неверные аргументы. */
#define KS_ERR_NOMEM   -3  /**< Ошибка выделения памяти пула. */
#define KS_ERR_CONN    -4  /**< В соединении отказано / узел недоступен. */
#define KS_ERR_TIMEOUT -5  /**< Истекло время ожидания операции. */
///@}

/* =========================================================================
 * 3. Типы данных
 * ========================================================================= */

/**
 * @brief Непрозрачный дескриптор сокета.
 * 
 * @details Внутренняя структура скрыта в платформенных исходниках (в internal.h).
 * Для пользователя API это просто указатель, обеспечивающий строгую инкапсуляцию
 * работы с ядерными объектами (WSK в Windows, sock в Linux).
 */
typedef struct KS_SOCKET KS_SOCKET;

/* =========================================================================
 * 4. Базовый жизненный цикл (Функции: camelCase, Аргументы: PascalCase)
 * ========================================================================= */

/**
 * @brief Создает новый сокет (выделяет ресурсы ядра).
 * 
 * @warning Вызов на IRQL >= DISPATCH_LEVEL приведет к BSOD (IRQL_NOT_LESS_OR_EQUAL).
 * 
 * @param[in] Protocol Транспортный протокол (KS_TCP или KS_UDP).
 * 
 * @return Указатель на структуру KS_SOCKET или NULL в случае нехватки памяти (NOMEM).
 */
KS_SOCKET* ksSocket(int Protocol);

/**
 * @brief Закрывает сокет и освобождает ресурсы ядра.
 * 
 * @warning Вызов на IRQL >= DISPATCH_LEVEL приведет к BSOD.
 * 
 * @param[in] Sock Указатель на сокет, полученный из ksSocket или ksAccept.
 */
void ksClose(KS_SOCKET* Sock);

/* =========================================================================
 * 5. Серверные функции (для сценариев U->K и K->K)
 * ========================================================================= */

/**
 * @brief Привязывает сокет к локальному IP-адресу и порту.
 * 
 * @warning Вызов на IRQL >= DISPATCH_LEVEL приведет к BSOD.
 * 
 * @param[in] Sock Указатель на сокет.
 * @param[in] Ip   Локальный IPv4-адрес строкой (например, "0.0.0.0").
 * @param[in] Port Локальный порт в host byte order.
 * 
 * @return KS_OK при успехе, иначе негативный код ошибки.
 */
int ksBind(KS_SOCKET* Sock, const char* Ip, unsigned short Port);

/**
 * @brief Переводит сокет TCP в режим прослушивания входящих соединений.
 * 
 * @warning Вызов на IRQL >= DISPATCH_LEVEL приведет к BSOD.
 * 
 * @param[in] Sock    Указатель на привязанный сокет (после ksBind).
 * @param[in] Backlog Максимальный размер очереди ожидающих подключений.
 * 
 * @return KS_OK при успехе, иначе код ошибки.
 */
int ksListen(KS_SOCKET* Sock, int Backlog);

/**
 * @brief Принимает входящее TCP-подключение (блокирующий вызов).
 * 
 * @warning Вызов на IRQL >= DISPATCH_LEVEL приведет к BSOD.
 * 
 * @param[in]  Sock       Слушающий сокет (после ksListen).
 * @param[out] NewSock    Возвращает новый сокет для общения с клиентом.
 * @param[out] ClientIp   Буфер (мин. 16 байт) для записи IP клиента (может быть NULL).
 * @param[out] ClientPort Переменная для записи порта клиента (может быть NULL).
 * 
 * @return KS_OK при успехе. Возвращает KS_ERR при ошибке или прерывании ожидания.
 */
int ksAccept(KS_SOCKET* Sock, KS_SOCKET** NewSock, char* ClientIp, unsigned short* ClientPort);

/* =========================================================================
 * 6. Клиентские и транспортные функции (TCP)
 * ========================================================================= */

/**
 * @brief Устанавливает TCP-соединение с удаленным узлом.
 * 
 * @warning Вызов на IRQL >= DISPATCH_LEVEL приведет к BSOD.
 * 
 * @param[in] Sock Указатель на сокет (KS_TCP).
 * @param[in] Ip   IPv4-адрес сервера строкой ("192.168.1.100").
 * @param[in] Port Порт сервера.
 * 
 * @return KS_OK или код ошибки (например, KS_ERR_CONN).
 */
int ksConnect(KS_SOCKET* Sock, const char* Ip, unsigned short Port);

/**
 * @brief Отправляет данные по TCP.
 * 
 * @warning BSOD Риск (Windows): Буфер Buf должен располагаться строго в невыгружаемой
 * памяти (NonPagedPool). Передача user-mode памяти приведет к PAGE_FAULT_IN_NONPAGED_AREA.
 * Функция должна вызываться на IRQL == PASSIVE_LEVEL.
 * 
 * @param[in] Sock Подключенный TCP-сокет.
 * @param[in] Buf  Указатель на буфер с данными.
 * @param[in] Len  Количество байт для отправки (> 0).
 * 
 * @return Количество отправленных байт или код ошибки.
 */
int ksSend(KS_SOCKET* Sock, const void* Buf, unsigned int Len);

/**
 * @brief Принимает данные по TCP (блокирует до получения минимум 1 байта).
 * 
 * @warning BSOD Риск (Windows): Буфер Buf должен располагаться строго в невыгружаемой
 * памяти (NonPagedPool). Функция должна вызываться на IRQL == PASSIVE_LEVEL.
 * 
 * @param[in]  Sock Подключенный TCP-сокет.
 * @param[out] Buf  Буфер для записи полученных данных.
 * @param[in]  Len  Размер буфера.
 * 
 * @return Количество прочитанных байт, 0 при закрытии соединения другой стороной, или ошибка.
 */
int ksRecv(KS_SOCKET* Sock, void* Buf, unsigned int Len);

/* =========================================================================
 * 7. Транспортные функции (UDP)
 * ========================================================================= */

/**
 * @brief Отправляет UDP-датаграмму заданному узлу.
 * 
 * @warning BSOD Риск (Windows): Буфер Buf должен располагаться строго в невыгружаемой
 * памяти (NonPagedPool). Функция должна вызываться на IRQL == PASSIVE_LEVEL.
 * 
 * @param[in] Sock UDP-сокет.
 * @param[in] Buf  Указатель на данные.
 * @param[in] Len  Длина данных (не более ~65507 байт).
 * @param[in] Ip   IP-адрес получателя.
 * @param[in] Port Порт получателя.
 * 
 * @return Количество отправленных байт или код ошибки.
 */
int ksSendTo(KS_SOCKET* Sock, const void* Buf, unsigned int Len, const char* Ip, unsigned short Port);

/**
 * @brief Принимает UDP-датаграмму и записывает адрес отправителя.
 * 
 * @warning BSOD Риск (Windows): Буфер Buf должен располагаться строго в невыгружаемой
 * памяти (NonPagedPool). Функция должна вызываться на IRQL == PASSIVE_LEVEL.
 * 
 * @param[in]  Sock    UDP-сокет (обычно после ksBind).
 * @param[out] Buf     Буфер для записи.
 * @param[in]  Len     Размер буфера Buf.
 * @param[out] SrcIp   Буфер (мин. 16 байт) для IP отправителя (может быть NULL).
 * @param[out] SrcPort Порт отправителя (может быть NULL).
 * 
 * @return Количество полученных байт или код ошибки.
 */
int ksRecvFrom(KS_SOCKET* Sock, void* Buf, unsigned int Len, char* SrcIp, unsigned short* SrcPort);

#ifdef __cplusplus
}
#endif

#endif /* KS_API_H */