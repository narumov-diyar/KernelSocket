/**
 * @file driver_entry.c
 * @brief Точка входа драйвера и встроенный тестовый стенд.
 *
 * @details
 * Демонстрирует использование API KernelSocket для создания TCP и UDP серверов
 * и клиентов в режиме ядра. Реализует 4 независимых системных потока для
 * параллельного тестирования протоколов. Поддерживает мягкую выгрузку
 * через паттерн "Loopback Wake-up".
 */

#include <ntddk.h>
#include <ntstrsafe.h>
#include "ks_windows_internal.h"
#include "../../include/ks_api.h"

/* =========================================================================
 * [НАСТРОЙКИ ТЕСТОВОГО СТЕНДА]
 * Изменяйте эти параметры для переключения между сценариями тестирования.
 * ========================================================================= */

/** @brief Локальный порт для TCP-сервера и целевой порт для TCP-клиента */
#define TEST_PORT_TCP 9000

/** @brief Локальный порт для UDP-сервера и целевой порт для UDP-клиента */
#define TEST_PORT_UDP 9001

/**
 * @brief IP-адрес целевого узла (куда стучатся клиенты).
 *
 * @note
 * - Для локального теста (Kernel->Kernel на одной машине): "127.0.0.1"
 * - Для сетевого теста (Win->Linux): IP-адрес Linux-машины (например, "26.175.255.229")
 */
#define TARGET_IP "26.217.126.117"

/* ========================================================================= */

/** @brief Глобальный флаг для безопасной остановки всех потоков при выгрузке */
BOOLEAN g_IsUnloading = FALSE;


/**
 * @brief Форматированный вывод логов в отладчик (DebugView/WinDbg).
 *
 * @param Tag Строковый тег подсистемы (например, "TCP-СЕРВЕР").
 * @param Format Строка формата (printf-style).
 * @param ... Аргументы для форматирования.
 */
static void KsLog(const char* Tag, const char* Format, ...) {
    LARGE_INTEGER SysTime, LocalTime;
    TIME_FIELDS TimeFields;
    char MessageBuffer[256];
    va_list Args;

    KeQuerySystemTime(&SysTime);
    ExSystemTimeToLocalTime(&SysTime, &LocalTime);
    RtlTimeToTimeFields(&LocalTime, &TimeFields);

    va_start(Args, Format);
    RtlStringCbVPrintfA(MessageBuffer, sizeof(MessageBuffer), Format, Args);
    va_end(Args);

    DbgPrint("[KS-LOG] [%02hd:%02hd:%02hd] [%s] %s\n",
        TimeFields.Hour, TimeFields.Minute, TimeFields.Second, Tag, MessageBuffer);
}

/**
 * @brief Получение текущего локального времени в виде строки.
 *
 * @param Buffer Указатель на буфер для записи (формат: ЧЧ:ММ:СС).
 * @param BufferSize Размер буфера.
 */
static void GetTimeStr(char* Buffer, size_t BufferSize) {
    LARGE_INTEGER SysTime, LocalTime;
    TIME_FIELDS TimeFields;

    KeQuerySystemTime(&SysTime);
    ExSystemTimeToLocalTime(&SysTime, &LocalTime);
    RtlTimeToTimeFields(&LocalTime, &TimeFields);

    RtlStringCbPrintfA(Buffer, BufferSize, "%02hd:%02hd:%02hd",
        TimeFields.Hour, TimeFields.Minute, TimeFields.Second);
}

/**
 * @brief Системный поток: Ядерный TCP-Сервер.
 * @details Надежный обмен. Принимает подключение, читает данные, отправляет квитанцию (ACK).
 *
 * @param Context Не используется.
 */
static void TcpServerThread(PVOID Context) {
    KS_SOCKET* ServerSock;
    KS_SOCKET* ClientSock;
    char ClientIp[16] = { 0 };
    unsigned short ClientPort = 0;
    char Buffer[256] = { 0 };
    char ReplyMsg[256] = { 0 };
    char TimeStr[16] = { 0 };
    int Bytes;

    UNREFERENCED_PARAMETER(Context);

    KsLog("TCP-СЕРВЕР", "Ожидание подключений на порту %d...", TEST_PORT_TCP);

    ServerSock = ksSocket(KS_TCP);
    if (!ServerSock) PsTerminateSystemThread(STATUS_SUCCESS);

    if (ksBind(ServerSock, "0.0.0.0", TEST_PORT_TCP) != KS_OK || ksListen(ServerSock, 1) != KS_OK) {
        KsLog("TCP-ОШИБКА", "Порт %d занят или сбой ksListen!", TEST_PORT_TCP);
        ksClose(ServerSock);
        PsTerminateSystemThread(STATUS_SUCCESS);
    }

    while (!g_IsUnloading) {
        if (ksAccept(ServerSock, &ClientSock, ClientIp, &ClientPort) == KS_OK) {

            /* Мгновенный выход, если поток был разбужен для выгрузки драйвера */
            if (g_IsUnloading) { ksClose(ClientSock); break; }

            KsLog("TCP-СЕРВЕР", "Подключение от %s:%d", ClientIp, ClientPort);

            Bytes = ksRecv(ClientSock, Buffer, sizeof(Buffer) - 1);
            if (Bytes > 0) {
                Buffer[Bytes] = '\0';
                KsLog("TCP-СЕРВЕР", "Получено: '%s'", Buffer);

                GetTimeStr(TimeStr, sizeof(TimeStr));
                RtlStringCbPrintfA(ReplyMsg, sizeof(ReplyMsg),
                    "ACK [Time: %s | Bytes received: %d]", TimeStr, Bytes);

                ksSend(ClientSock, ReplyMsg, (unsigned int)strlen(ReplyMsg) + 1);
                KsLog("TCP-СЕРВЕР", "Отправлена квитанция: %s", ReplyMsg);

                /* Задержка (2 сек) для предотвращения Race Condition (TCP RST) при закрытии */
                LARGE_INTEGER Delay;
                Delay.QuadPart = -20000000LL;
                KeDelayExecutionThread(KernelMode, FALSE, &Delay);
            }
            ksClose(ClientSock);
        }
        else {
            break; /* Выход при ошибке или прерывании */
        }
    }

    ksClose(ServerSock);
    KsLog("TCP-СЕРВЕР", "Успешно остановлен.");
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/**
 * @brief Системный поток: Ядерный TCP-Клиент.
 * @details Генерирует N подключений к серверу, отправляет данные и ждет квитанцию.
 *
 * @param Context Не используется.
 */
static void TcpClientThread(PVOID Context) {
    KS_SOCKET* ClientSock;
    LARGE_INTEGER Timeout, Tick;
    char Msg[256] = { 0 };
    char Reply[256] = { 0 };
    char TimeStr[16] = { 0 };
    int Bytes, N;

    UNREFERENCED_PARAMETER(Context);

    /* Задержка 5 сек перед стартом клиента, чтобы серверы успели запуститься */
    Timeout.QuadPart = -50000000LL;
    KeDelayExecutionThread(KernelMode, FALSE, &Timeout);

    /* Случайное число пакетов от 3 до 7 на основе системных тиков */
    KeQueryTickCount(&Tick);
    N = 3 + (Tick.LowPart % 5);

    KsLog("TCP-КЛИЕНТ", "Запуск! Будет отправлено %d пакетов на %s:%d", N, TARGET_IP, TEST_PORT_TCP);

    for (int i = 1; i <= N; i++) {
        if (g_IsUnloading) break;

        ClientSock = ksSocket(KS_TCP);
        if (!ClientSock) break;

        if (ksConnect(ClientSock, TARGET_IP, TEST_PORT_TCP) == KS_OK) {
            GetTimeStr(TimeStr, sizeof(TimeStr));
            RtlStringCbPrintfA(Msg, sizeof(Msg), "[Packet %d/%d | Time: %s] TCP Test from Windows", i, N, TimeStr);

            ksSend(ClientSock, Msg, (unsigned int)strlen(Msg) + 1);
            KsLog("TCP-КЛИЕНТ", "Отправлено [%d/%d]: '%s'", i, N, Msg);

            Bytes = ksRecv(ClientSock, Reply, sizeof(Reply) - 1);
            if (Bytes > 0) {
                Reply[Bytes] = '\0';
                KsLog("TCP-КЛИЕНТ", "Ответ узла: '%s'", Reply);
            }
        }
        else {
            KsLog("TCP-ОШИБКА", "Сбой подключения к %s:%d", TARGET_IP, TEST_PORT_TCP);
        }
        ksClose(ClientSock);

        /* Пауза 4 секунды между отправками пакетов */
        Timeout.QuadPart = -40000000LL;
        KeDelayExecutionThread(KernelMode, FALSE, &Timeout);
    }

    KsLog("TCP-КЛИЕНТ", "Тестирование завершено.");
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/**
 * @brief Системный поток: Ядерный UDP-Сервер.
 * @details Демонстрирует односторонний прием (Fire-and-Forget). Просто логирует датаграммы.
 *
 * @param Context Не используется.
 */
static void UdpServerThread(PVOID Context) {
    KS_SOCKET* ServerSock;
    char ClientIp[16] = { 0 };
    unsigned short ClientPort = 0;
    char Buffer[256] = { 0 };
    int Bytes;

    UNREFERENCED_PARAMETER(Context);

    KsLog("UDP-СЕРВЕР", "Ожидание датаграмм на порту %d...", TEST_PORT_UDP);

    ServerSock = ksSocket(KS_UDP);
    if (!ServerSock) PsTerminateSystemThread(STATUS_SUCCESS);

    if (ksBind(ServerSock, "0.0.0.0", TEST_PORT_UDP) != KS_OK) {
        KsLog("UDP-ОШИБКА", "Порт %d занят!", TEST_PORT_UDP);
        ksClose(ServerSock);
        PsTerminateSystemThread(STATUS_SUCCESS);
    }

    while (!g_IsUnloading) {
        Bytes = ksRecvFrom(ServerSock, Buffer, sizeof(Buffer) - 1, ClientIp, &ClientPort);
        if (Bytes > 0) {
            if (g_IsUnloading) break;

            Buffer[Bytes] = '\0';
            KsLog("UDP-СЕРВЕР", "Получено от %s:%d: '%s'", ClientIp, ClientPort, Buffer);

            /* UDP - протокол без гарантии доставки. Сервер только принимает. */
        }
        else {
            break;
        }
    }

    ksClose(ServerSock);
    KsLog("UDP-СЕРВЕР", "Успешно остановлен.");
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/**
 * @brief Системный поток: Ядерный UDP-Клиент.
 * @details Демонстрирует UDP-стриминг. Отправляет N датаграмм без ожидания квитанций.
 *
 * @param Context Не используется.
 */
static void UdpClientThread(PVOID Context) {
    KS_SOCKET* ClientSock;
    LARGE_INTEGER Timeout, Tick;
    char Msg[256] = { 0 };
    char TimeStr[16] = { 0 };
    int N;

    UNREFERENCED_PARAMETER(Context);

    /* Задержка 6 сек перед стартом клиента */
    Timeout.QuadPart = -60000000LL;
    KeDelayExecutionThread(KernelMode, FALSE, &Timeout);

    KeQueryTickCount(&Tick);
    N = 3 + ((Tick.LowPart >> 4) % 5);

    KsLog("UDP-КЛИЕНТ", "Запуск! Стриминг %d датаграмм на %s:%d", N, TARGET_IP, TEST_PORT_UDP);

    ClientSock = ksSocket(KS_UDP);
    if (!ClientSock) PsTerminateSystemThread(STATUS_SUCCESS);

    for (int i = 1; i <= N; i++) {
        if (g_IsUnloading) break;

        GetTimeStr(TimeStr, sizeof(TimeStr));
        RtlStringCbPrintfA(Msg, sizeof(Msg), "UDP-DATAGRAM [%d/%d | Time: %s] from Windows", i, N, TimeStr);

        if (ksSendTo(ClientSock, Msg, (unsigned int)strlen(Msg) + 1, TARGET_IP, TEST_PORT_UDP) > 0) {
            KsLog("UDP-КЛИЕНТ", "Отправлено [%d/%d]: '%s'", i, N, Msg);
        }
        else {
            KsLog("UDP-ОШИБКА", "Сбой отправки к %s:%d", TARGET_IP, TEST_PORT_UDP);
        }

        /* Пауза 4 секунды между отправками датаграмм */
        Timeout.QuadPart = -40000000LL;
        KeDelayExecutionThread(KernelMode, FALSE, &Timeout);
    }

    ksClose(ClientSock);
    KsLog("UDP-КЛИЕНТ", "Тестирование завершено.");
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* =========================================================================
 * Жизненный цикл драйвера
 * ========================================================================= */

/**
 * @brief Функция выгрузки драйвера (срабатывает при 'sc stop').
 * @details Реализует безопасное завершение ожидающих потоков через Loopback Wake-up.
 *
 * @param DriverObject Объект драйвера.
 */
DRIVER_UNLOAD KsDriverUnload;

void KsDriverUnload(_In_ PDRIVER_OBJECT DriverObject) {
    KS_SOCKET* WakeSock;
    LARGE_INTEGER Delay;
    UNREFERENCED_PARAMETER(DriverObject);

    KsLog("ЯДРО", "===========================================");
    KsLog("ЯДРО", "Команда 'sc stop' получена. Мягкая выгрузка...");

    g_IsUnloading = TRUE;

    /* Будим TCP-сервер локальным подключением */
    WakeSock = ksSocket(KS_TCP);
    if (WakeSock) { ksConnect(WakeSock, "127.0.0.1", TEST_PORT_TCP); ksClose(WakeSock); }

    /* Будим UDP-сервер фиктивной датаграммой */
    WakeSock = ksSocket(KS_UDP);
    if (WakeSock) { ksSendTo(WakeSock, "X", 1, "127.0.0.1", TEST_PORT_UDP); ksClose(WakeSock); }

    /* Задержка 0.5 сек для корректного освобождения ресурсов потоками */
    Delay.QuadPart = -5000000LL;
    KeDelayExecutionThread(KernelMode, FALSE, &Delay);

    KsWskCleanup();
    KsLog("ЯДРО", "Драйвер успешно выгружен.");
    KsLog("ЯДРО", "===========================================\n");
}

/**
 * @brief Точка входа драйвера (срабатывает при 'sc start').
 *
 * @param DriverObject Объект драйвера.
 * @param RegistryPath Путь в реестре (не используется).
 * @return NTSTATUS STATUS_SUCCESS или код ошибки инициализации WSK.
 */
DRIVER_INITIALIZE DriverEntry;

NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath) {
    NTSTATUS Status;
    HANDLE h1, h2, h3, h4;

    UNREFERENCED_PARAMETER(RegistryPath);

    DriverObject->DriverUnload = KsDriverUnload;

    KsLog("ЯДРО", "===========================================");
    KsLog("ЯДРО", "Загрузка драйвера KernelSocket...");

    Status = KsWskInit();
    if (!NT_SUCCESS(Status)) {
        KsLog("ОШИБКА", "Сбой KsWskInit: 0x%08X", Status);
        return Status;
    }

    /* 1. Запуск серверных потоков */
    // if (NT_SUCCESS(PsCreateSystemThread(&h1, THREAD_ALL_ACCESS, NULL, NULL, NULL, TcpServerThread, NULL))) ZwClose(h1);
    // if (NT_SUCCESS(PsCreateSystemThread(&h3, THREAD_ALL_ACCESS, NULL, NULL, NULL, UdpServerThread, NULL))) ZwClose(h3);

    /*
     * [НАСТРОЙКА: СЕТЕВЫЕ КЛИЕНТЫ]
     * Если вы тестируете взаимодействие по локальной сети и хотите,
     * чтобы этот драйвер работал ТОЛЬКО как сервер, закомментируйте
     * следующие 2 строки (создание клиентов).
     */
    if (NT_SUCCESS(PsCreateSystemThread(&h2, THREAD_ALL_ACCESS, NULL, NULL, NULL, TcpClientThread, NULL))) ZwClose(h2);
    if (NT_SUCCESS(PsCreateSystemThread(&h4, THREAD_ALL_ACCESS, NULL, NULL, NULL, UdpClientThread, NULL))) ZwClose(h4);

    KsLog("ЯДРО", "Все тестовые потоки успешно инициированы.");
    KsLog("ЯДРО", "===========================================");
    return STATUS_SUCCESS;
}