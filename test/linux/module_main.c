/**
 * @file module_main.c
 * @brief Точка входа модуля Linux и встроенный тестовый стенд.
 * 
 * @details 
 * Демонстрирует использование API KernelSocket в ядре Linux. Запускает 4 потока
 * (kthreads) для параллельного тестирования TCP (с квитанциями) и UDP (стриминг).
 * Реализует паттерн безопасной выгрузки модуля через kernel_sock_shutdown.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kthread.h> 
#include <linux/delay.h>  
#include <linux/net.h>     
#include <linux/timekeeping.h>
#include <linux/random.h>
#include "ks_linux_internal.h" 
#include "../../include/ks_api.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Kernel Crew");
MODULE_DESCRIPTION("KernelSocket Test Module - Full Stress Test");

/* =========================================================================
 * [НАСТРОЙКИ ТЕСТОВОГО СТЕНДА]
 * Изменяйте эти параметры для переключения между сценариями тестирования.
 * ========================================================================= */

#define TEST_PORT_TCP 9000
#define TEST_PORT_UDP 9001
#define TARGET_IP "26.24.120.244" /* Замените на IP напарника (Windows) для сетевого теста */

/* ========================================================================= */

/* Глобальные указатели на сокеты серверов (нужны для экстренного прерывания) */
static KS_SOCKET *G_TcpServerSock = NULL;
static KS_SOCKET *G_UdpServerSock = NULL;

/* Глобальные указатели на системные потоки (kthreads) */
static struct task_struct *t_tcp_srv, *t_udp_srv, *t_tcp_cli, *t_udp_cli;

/* =========================================================================
 * Вспомогательная функция: Вывод логов в едином формате (как в Windows/Python)
 * Формат: [KS-LOG] [ЧЧ:ММ:СС] [ТЕГ] Сообщение
 * ========================================================================= */
static void ks_get_time_str(char *buffer, size_t size) {
    struct timespec64 ts;
    struct tm tm;

    ktime_get_real_ts64(&ts);
    /* 0 означает смещение по UTC. Для простоты используем его. */
    time64_to_tm(ts.tv_sec, 0, &tm); 
    
    snprintf(buffer, size, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
}

static void ks_log(const char *tag, const char *fmt, ...) {
    char time_str[16];
    char msg_buf[256];
    va_list args;

    ks_get_time_str(time_str, sizeof(time_str));

    va_start(args, fmt);
    vsnprintf(msg_buf, sizeof(msg_buf), fmt, args);
    va_end(args);

    /* Вывод в dmesg */
    pr_info("[KS-LOG] [%s] [%s] %s\n", time_str, tag, msg_buf);
}

/* =========================================================================
 * Поток 1: Ядерный TCP-Сервер (Режим: Надежный обмен с ACK)
 * ========================================================================= */
static int tcp_server_thread(void *arg) {
    KS_SOCKET *ClientSock;
    char ClientIp[16] = {0};
    unsigned short ClientPort = 0;
    char Buffer[256] = {0};
    char ReplyMsg[256] = {0};
    char TimeStr[16] = {0};
    int Bytes;

    ks_log("TCP-СЕРВЕР", "Ожидание подключений на порту %d...", TEST_PORT_TCP);
    
    G_TcpServerSock = ksSocket(KS_TCP);
    if (!G_TcpServerSock) return -1;

    if (ksBind(G_TcpServerSock, "0.0.0.0", TEST_PORT_TCP) != KS_OK || ksListen(G_TcpServerSock, 1) != KS_OK) {
        ks_log("TCP-ОШИБКА", "Порт %d занят или сбой ksListen!", TEST_PORT_TCP);
        ksClose(G_TcpServerSock);
        G_TcpServerSock = NULL;
        return -1;
    }

    while (!kthread_should_stop()) {
        if (ksAccept(G_TcpServerSock, &ClientSock, ClientIp, &ClientPort) == KS_OK) {
            
            if (kthread_should_stop()) { ksClose(ClientSock); break; }

            ks_log("TCP-СЕРВЕР", "Подключение от %s:%d", ClientIp, ClientPort);

            Bytes = ksRecv(ClientSock, Buffer, sizeof(Buffer) - 1);
            if (Bytes > 0) {
                Buffer[Bytes] = '\0';
                ks_log("TCP-СЕРВЕР", "Получено: '%s'", Buffer);

                ks_get_time_str(TimeStr, sizeof(TimeStr));
                snprintf(ReplyMsg, sizeof(ReplyMsg), "ACK [Time: %s | Bytes received: %d]", TimeStr, Bytes);
                
                ksSend(ClientSock, ReplyMsg, strlen(ReplyMsg) + 1);
                ks_log("TCP-СЕРВЕР", "Отправлена квитанция: %s", ReplyMsg);

                /* Задержка 2 сек (Fix Race Condition при закрытии) */
                msleep(2000); 
            }
            ksClose(ClientSock);
        } else {
            /* Вызывается, если kernel_sock_shutdown прервал ksAccept */
            msleep(100); 
        }
    }
    
    if (G_TcpServerSock) { ksClose(G_TcpServerSock); G_TcpServerSock = NULL; }
    ks_log("TCP-СЕРВЕР", "Успешно остановлен.");
    
    /* Ожидание сигнала kthread_stop от функции выгрузки */
    while (!kthread_should_stop()) msleep(100);
    return 0;
}

/* =========================================================================
 * Поток 2: Ядерный TCP-Клиент
 * ========================================================================= */
static int tcp_client_thread(void *arg) {
    KS_SOCKET *ClientSock;
    char Msg[256] = {0};
    char Reply[256] = {0};
    char TimeStr[16] = {0};
    int Bytes, N;
    int i;

    /* Ждем 5 сек, пока поднимутся серверы */
    msleep(5000); 

    /* Генерируем случайное число пакетов от 3 до 7 */
    N = 3 + (get_random_u32() % 5);

    ks_log("TCP-КЛИЕНТ", "Запуск! Будет отправлено %d пакетов на %s:%d", N, TARGET_IP, TEST_PORT_TCP);
    
    for (i = 1; i <= N; i++) {
        if (kthread_should_stop()) break;

        ClientSock = ksSocket(KS_TCP);
        if (!ClientSock) break;

        if (ksConnect(ClientSock, TARGET_IP, TEST_PORT_TCP) == KS_OK) {
            ks_get_time_str(TimeStr, sizeof(TimeStr));
            snprintf(Msg, sizeof(Msg), "[Packet %d/%d | Time: %s] TCP Test from Linux", i, N, TimeStr);
            
            ksSend(ClientSock, Msg, strlen(Msg) + 1);
            ks_log("TCP-КЛИЕНТ", "Отправлено [%d/%d]: '%s'", i, N, Msg);

            Bytes = ksRecv(ClientSock, Reply, sizeof(Reply) - 1);
            if (Bytes > 0) {
                Reply[Bytes] = '\0';
                ks_log("TCP-КЛИЕНТ", "Ответ узла: '%s'", Reply);
            }
        } else {
            ks_log("TCP-ОШИБКА", "Сбой подключения к %s:%d", TARGET_IP, TEST_PORT_TCP);
        }
        ksClose(ClientSock);

        /* Задержка 4 секунды между отправками */
        msleep(4000);
    }
    
    ks_log("TCP-КЛИЕНТ", "Тестирование завершено.");
    while (!kthread_should_stop()) msleep(100);
    return 0;
}

/* =========================================================================
 * Поток 3: Ядерный UDP-Сервер (Режим: Односторонний прием)
 * ========================================================================= */
static int udp_server_thread(void *arg) {
    char ClientIp[16] = {0};
    unsigned short ClientPort = 0;
    char Buffer[256] = {0};
    int Bytes;

    ks_log("UDP-СЕРВЕР", "Ожидание датаграмм на порту %d...", TEST_PORT_UDP);
    
    G_UdpServerSock = ksSocket(KS_UDP);
    if (!G_UdpServerSock) return -1;

    if (ksBind(G_UdpServerSock, "0.0.0.0", TEST_PORT_UDP) != KS_OK) {
        ks_log("UDP-ОШИБКА", "Порт %d занят!", TEST_PORT_UDP);
        ksClose(G_UdpServerSock);
        G_UdpServerSock = NULL;
        return -1;
    }

    while (!kthread_should_stop()) {
        Bytes = ksRecvFrom(G_UdpServerSock, Buffer, sizeof(Buffer) - 1, ClientIp, &ClientPort);
        if (Bytes > 0) {
            if (kthread_should_stop()) break;

            Buffer[Bytes] = '\0';
            ks_log("UDP-СЕРВЕР", "Получено от %s:%d: '%s'", ClientIp, ClientPort, Buffer);
            
            /* UDP - это Fire-And-Forget. Просто логируем. */
        } else {
            msleep(100);
        }
    }
    
    if (G_UdpServerSock) { ksClose(G_UdpServerSock); G_UdpServerSock = NULL; }
    ks_log("UDP-СЕРВЕР", "Успешно остановлен.");
    
    while (!kthread_should_stop()) msleep(100);
    return 0;
}

/* =========================================================================
 * Поток 4: Ядерный UDP-Клиент (Режим: Стриминг датаграмм)
 * ========================================================================= */
static int udp_client_thread(void *arg) {
    KS_SOCKET *ClientSock;
    char Msg[256] = {0};
    char TimeStr[16] = {0};
    int N;
    int i;

    /* Ждем 6 сек, чтобы логи в терминале не перемешались с TCP */
    msleep(6000); 

    N = 3 + (get_random_u32() % 5);

    ks_log("UDP-КЛИЕНТ", "Запуск! Стриминг %d датаграмм на %s:%d", N, TARGET_IP, TEST_PORT_UDP);

    ClientSock = ksSocket(KS_UDP);
    if (!ClientSock) return -1;

    for (i = 1; i <= N; i++) {
        if (kthread_should_stop()) break;

        ks_get_time_str(TimeStr, sizeof(TimeStr));
        snprintf(Msg, sizeof(Msg), "UDP-DATAGRAM [%d/%d | Time: %s] from Linux", i, N, TimeStr);

        if (ksSendTo(ClientSock, Msg, strlen(Msg) + 1, TARGET_IP, TEST_PORT_UDP) > 0) {
            ks_log("UDP-КЛИЕНТ", "Отправлено [%d/%d]: '%s'", i, N, Msg);
        } else {
            ks_log("UDP-ОШИБКА", "Сбой отправки к %s:%d", TARGET_IP, TEST_PORT_UDP);
        }

        msleep(4000);
    }

    ksClose(ClientSock);
    ks_log("UDP-КЛИЕНТ", "Тестирование завершено.");
    
    while (!kthread_should_stop()) msleep(100);
    return 0;
}

/* =========================================================================
 * Жизненный цикл драйвера (Linux)
 * ========================================================================= */

static int __init ks_module_init(void) {
    ks_log("ЯДРО", "===========================================");
    ks_log("ЯДРО", "Загрузка модуля KernelSocket (Linux)...");

    /* В отличие от Windows, в Linux сетевой стек (sock_create) доступен сразу */

    /* 1. Запуск серверных потоков */
    // t_tcp_srv = kthread_run(tcp_server_thread, NULL, "ks_tcp_srv");
    // t_udp_srv = kthread_run(udp_server_thread, NULL, "ks_udp_srv");
    
    /* 
     * [НАСТРОЙКА: СЕТЕВЫЕ КЛИЕНТЫ]
     * Если вы тестируете взаимодействие по локальной сети и хотите, 
     * чтобы этот драйвер работал ТОЛЬКО как сервер, закомментируйте 
     * следующие 2 строки (создание клиентов).
     */
    t_tcp_cli = kthread_run(tcp_client_thread, NULL, "ks_tcp_cli");
    t_udp_cli = kthread_run(udp_client_thread, NULL, "ks_udp_cli");

    ks_log("ЯДРО", "Все тестовые потоки успешно инициированы.");
    ks_log("ЯДРО", "===========================================");
    return 0;
}

static void __exit ks_module_exit(void) {
    ks_log("ЯДРО", "===========================================");
    ks_log("ЯДРО", "Команда 'rmmod' получена. Мягкая выгрузка...");

    /* 1. Экстренно обрываем сетевые вызовы (разблокируем accept и recvfrom).
     * Это Linux-аналог паттерна 'Loopback Wake-up' из Windows. */
    if (G_TcpServerSock && G_TcpServerSock->sock) {
        kernel_sock_shutdown(G_TcpServerSock->sock, SHUT_RDWR);
    }
    if (G_UdpServerSock && G_UdpServerSock->sock) {
        kernel_sock_shutdown(G_UdpServerSock->sock, SHUT_RDWR);
    }

    /* 2. Официально завершаем потоки (kthread_should_stop станет TRUE) */
    if (t_tcp_cli) kthread_stop(t_tcp_cli);
    if (t_udp_cli) kthread_stop(t_udp_cli);
    if (t_tcp_srv) kthread_stop(t_tcp_srv);
    if (t_udp_srv) kthread_stop(t_udp_srv);

    ks_log("ЯДРО", "Модуль успешно выгружен. Память очищена.");
    ks_log("ЯДРО", "===========================================\n");
}

module_init(ks_module_init);
module_exit(ks_module_exit);