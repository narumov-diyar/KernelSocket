/**
 * @file ks_windows.c
 * @brief Реализация KernelSocket для Windows через Winsock Kernel (WSK).
 *
 * @details
 * Файл реализует публичный API из `ks_api.h` для режима ядра Windows.
 * Все сетевые операции выполняются синхронно поверх IRP-оберток, что делает
 * поведение функций предсказуемым для кроссплатформенного API KernelSocket.
 * Внутри используется паттерн "Ленивая инициализация" для WSK-сокетов.
 */

#include "ks_windows_internal.h"

#pragma comment(lib, "netio.lib")

/* =========================================================================
 * Глобальные переменные подсистемы WSK
 * ========================================================================= */

/** @brief Таблица диспетчеризации клиента WSK (версия 1.0). */
static const WSK_CLIENT_DISPATCH g_WskClientDispatch = { MAKE_WSK_VERSION(1, 0), 0, NULL };

/** @brief Дескриптор регистрации клиента WSK. */
WSK_REGISTRATION g_WskRegistration;

/** @brief Провайдер NPI, содержащий указатели на функции подсистемы WSK. */
WSK_PROVIDER_NPI g_WskProvider;

/** @brief Флаг успешной инициализации подсистемы WSK. */
BOOLEAN          g_WskReady = FALSE;

/* =========================================================================
 * Вспомогательные IRP контексты
 * ========================================================================= */

/**
 * @brief Базовый контекст для синхронного ожидания завершения IRP.
 */
typedef struct _KS_IRP_CTX {
    KEVENT   Event;   /**< Событие для блокировки потока до завершения IRP. */
    NTSTATUS Status;  /**< Итоговый статус выполнения операции. */
} KS_IRP_CTX;

/**
 * @brief Расширенный контекст IRP с дополнительным полем результата.
 *
 * @details
 * Поле `Information` используется для возврата указателя на сокет (при создании) 
 * или количества байт (при приеме/передаче) из `IoStatus.Information`.
 */
typedef struct _KS_IRP_CTX_EX {
    KS_IRP_CTX Base;         /**< Базовый контекст с событием и статусом. */
    ULONG_PTR  Information;  /**< Дополнительная информация от I/O менеджера. */
} KS_IRP_CTX_EX;

/**
 * @brief Функция завершения (Completion routine) для базового IRP.
 * 
 * @param[in] DeviceObject Указатель на объект устройства (не используется).
 * @param[in] Irp          Указатель на завершенный IRP.
 * @param[in] Context      Пользовательский контекст (KS_IRP_CTX).
 * 
 * @return Всегда STATUS_MORE_PROCESSING_REQUIRED для предотвращения удаления IRP системой.
 */
static NTSTATUS NTAPI KsIrpCompletion(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context) {
    KS_IRP_CTX* Ctx = (KS_IRP_CTX*)Context;
    UNREFERENCED_PARAMETER(DeviceObject);
    Ctx->Status = Irp->IoStatus.Status;
    KeSetEvent(&Ctx->Event, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

/**
 * @brief Функция завершения (Completion routine) для расширенного IRP.
 * 
 * @param[in] DeviceObject Указатель на объект устройства (не используется).
 * @param[in] Irp          Указатель на завершенный IRP.
 * @param[in] Context      Пользовательский контекст (KS_IRP_CTX_EX).
 * 
 * @return Всегда STATUS_MORE_PROCESSING_REQUIRED.
 */
static NTSTATUS NTAPI KsIrpCompletionEx(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context) {
    KS_IRP_CTX_EX* Ctx = (KS_IRP_CTX_EX*)Context;
    UNREFERENCED_PARAMETER(DeviceObject);
    Ctx->Base.Status = Irp->IoStatus.Status;
    Ctx->Information = Irp->IoStatus.Information;
    KeSetEvent(&Ctx->Base.Event, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

/**
 * @brief Выделяет IRP и связывает его с базовым контекстом ожидания.
 * 
 * @param[in] Ctx Указатель на инициализируемый базовый контекст.
 * 
 * @return Указатель на выделенный IRP или NULL при ошибке.
 */
static PIRP KsAllocateIrp(KS_IRP_CTX* Ctx) {
    PIRP Irp;
    KeInitializeEvent(&Ctx->Event, NotificationEvent, FALSE);
    Ctx->Status = STATUS_UNSUCCESSFUL;
    Irp = IoAllocateIrp(1, FALSE);
    if (Irp) IoSetCompletionRoutine(Irp, KsIrpCompletion, Ctx, TRUE, TRUE, TRUE);
    return Irp;
}

/**
 * @brief Выделяет IRP и связывает его с расширенным контекстом ожидания.
 * 
 * @param[in] Ctx Указатель на инициализируемый расширенный контекст.
 * 
 * @return Указатель на выделенный IRP или NULL при ошибке.
 */
static PIRP KsAllocateIrpEx(KS_IRP_CTX_EX* Ctx) {
    PIRP Irp;
    KeInitializeEvent(&Ctx->Base.Event, NotificationEvent, FALSE);
    Ctx->Base.Status = STATUS_UNSUCCESSFUL;
    Ctx->Information = 0;
    Irp = IoAllocateIrp(1, FALSE);
    if (Irp) IoSetCompletionRoutine(Irp, KsIrpCompletionEx, Ctx, TRUE, TRUE, TRUE);
    return Irp;
}

/**
 * @brief Блокирующе ожидает завершения IRP и освобождает его.
 *
 * @param[in] Irp Указатель на IRP для ожидания.
 * @param[in] Ctx Базовый контекст, связанный с IRP.
 * 
 * @return Итоговый `NTSTATUS` завершенной сетевой операции.
 */
static NTSTATUS KsWaitIrp(PIRP Irp, KS_IRP_CTX* Ctx) {
    KeWaitForSingleObject(&Ctx->Event, Executive, KernelMode, FALSE, NULL);
    IoFreeIrp(Irp);
    return Ctx->Status;
}

/* =========================================================================
 * IP Конвертеры
 * ========================================================================= */

/**
 * @brief Преобразует строковый IPv4 и порт в `SOCKADDR_IN`.
 *
 * @param[in]  Ip      IPv4-адрес в формате строки (например, "127.0.0.1").
 * @param[in]  Port    Порт в host byte order.
 * @param[out] OutAddr Буфер для записи бинарного адреса.
 * 
 * @return `STATUS_SUCCESS` при успехе, иначе `STATUS_INVALID_PARAMETER`.
 */
static NTSTATUS KsStringToSockaddr(const char* Ip, unsigned short Port, SOCKADDR_IN* OutAddr) {
    WCHAR WideIp[16] = { 0 };
    PCWSTR Terminator;
    int i;
    for (i = 0; i < 15 && Ip[i] != '\0'; i++) WideIp[i] = (WCHAR)(unsigned char)Ip[i];
    
    if (!NT_SUCCESS(RtlIpv4StringToAddressW(WideIp, TRUE, &Terminator, (struct in_addr*)&OutAddr->sin_addr)))
        return STATUS_INVALID_PARAMETER;

    OutAddr->sin_family = AF_INET;
    OutAddr->sin_port = RtlUshortByteSwap(Port);
    return STATUS_SUCCESS;
}

/**
 * @brief Преобразует бинарный `SOCKADDR_IN` в строку IPv4 и порт.
 *
 * @param[in]  Addr    Указатель на бинарный адрес источника.
 * @param[out] OutIp   Буфер для строки IPv4 (минимум 16 байт, может быть `NULL`).
 * @param[out] OutPort Буфер для записи порта в host byte order (может быть `NULL`).
 */
static void KsSockaddrToString(SOCKADDR_IN* Addr, char* OutIp, unsigned short* OutPort) {
    if (OutIp) {
        WCHAR WideIp[INET_ADDRSTRLEN] = { 0 };
        int i;
        RtlIpv4AddressToStringW(&Addr->sin_addr, WideIp);
        for (i = 0; i < 15 && WideIp[i] != L'\0'; i++) OutIp[i] = (char)WideIp[i];
        OutIp[i] = '\0';
    }
    if (OutPort) *OutPort = RtlUshortByteSwap(Addr->sin_port);
}

/* =========================================================================
 * Инициализация WSK (Из DriverEntry)
 * ========================================================================= */

/**
 * @brief Инициализирует подсистему Winsock Kernel (WSK).
 * 
 * @details Вызывается однократно при загрузке драйвера (в DriverEntry).
 * 
 * @return STATUS_SUCCESS при успешной регистрации, иначе код ошибки ядра.
 */
NTSTATUS KsWskInit(void) {
    WSK_CLIENT_NPI ClientNpi = { NULL, &g_WskClientDispatch };
    NTSTATUS Status;

    Status = WskRegister(&ClientNpi, &g_WskRegistration);
    if (!NT_SUCCESS(Status)) return Status;

    Status = WskCaptureProviderNPI(&g_WskRegistration, WSK_INFINITE_WAIT, &g_WskProvider);
    if (!NT_SUCCESS(Status)) { WskDeregister(&g_WskRegistration); return Status; }

    g_WskReady = TRUE;
    return STATUS_SUCCESS;
}

/**
 * @brief Корректно завершает работу с подсистемой WSK.
 * 
 * @details Вызывается однократно при выгрузке драйвера (DriverUnload).
 */
void KsWskCleanup(void) {
    if (!g_WskReady) return;
    WskReleaseProviderNPI(&g_WskRegistration);
    WskDeregister(&g_WskRegistration);
    g_WskReady = FALSE;
}

/* =========================================================================
 * Внутренняя функция "Ленивой" инициализации WSK сокета
 * ========================================================================= */

/**
 * @brief Создает WSK-сокет по требованию и выполняет его привязку (Bind).
 *
 * @details
 * WSK требует указывать роль сокета (WSK_FLAG_LISTEN_SOCKET или WSK_FLAG_CONNECTION_SOCKET)
 * в момент создания. Данная функция откладывает реальное создание ядерного объекта WSK 
 * до тех пор, пока пользователь не вызовет ksConnect, ksListen или ksSendTo.
 *
 * @param[in,out] Sock     Внутренний дескриптор KernelSocket.
 * @param[in]     WskFlags Флаги создания WSK-сокета (`WSK_FLAG_*`).
 * 
 * @return `STATUS_SUCCESS` при успехе, иначе код ошибки WSK/NTSTATUS.
 */
static NTSTATUS KsLazyCreateWskSocket(KS_SOCKET* Sock, ULONG WskFlags) {
    KS_IRP_CTX_EX CtxEx;
    KS_IRP_CTX    Ctx;
    PIRP          Irp;
    NTSTATUS      Status;
    USHORT        SocketType = (Sock->Protocol == KS_TCP) ? SOCK_STREAM : SOCK_DGRAM;
    ULONG         IpProto    = (Sock->Protocol == KS_TCP) ? IPPROTO_TCP : IPPROTO_UDP;
    const WSK_PROVIDER_DISPATCH* ProvDisp = g_WskProvider.Dispatch;

    if (Sock->WskSocket != NULL) return STATUS_SUCCESS; /* Уже создан */

    /* Шаг 1: WskSocket */
    Irp = KsAllocateIrpEx(&CtxEx);
    if (!Irp) return STATUS_INSUFFICIENT_RESOURCES;

    Status = ProvDisp->WskSocket(g_WskProvider.Client, AF_INET, SocketType, IpProto, WskFlags,
                                 NULL, NULL, NULL, NULL, NULL, Irp);
    KeWaitForSingleObject(&CtxEx.Base.Event, Executive, KernelMode, FALSE, NULL);
    IoFreeIrp(Irp);

    if (!NT_SUCCESS(CtxEx.Base.Status)) return CtxEx.Base.Status;
    Sock->WskSocket = (PWSK_SOCKET)CtxEx.Information;

    /* Шаг 2: WskBind (Даже клиентам нужен bind на 0.0.0.0:0 в WSK) */
    if (!Sock->IsBound) {
        RtlZeroMemory(&Sock->LocalAddress, sizeof(Sock->LocalAddress));
        Sock->LocalAddress.sin_family = AF_INET;
        Sock->LocalAddress.sin_port = 0;
        Sock->LocalAddress.sin_addr.S_un.S_addr = INADDR_ANY;
    }

    Irp = KsAllocateIrp(&Ctx);
    if (!Irp) return STATUS_INSUFFICIENT_RESOURCES;

    if (Sock->Protocol == KS_TCP) {
        if (WskFlags == WSK_FLAG_LISTEN_SOCKET) {
            const WSK_PROVIDER_LISTEN_DISPATCH* Disp = (const WSK_PROVIDER_LISTEN_DISPATCH*)Sock->WskSocket->Dispatch;
            Status = Disp->WskBind(Sock->WskSocket, (PSOCKADDR)&Sock->LocalAddress, 0, Irp);
        } else {
            const WSK_PROVIDER_CONNECTION_DISPATCH* Disp = (const WSK_PROVIDER_CONNECTION_DISPATCH*)Sock->WskSocket->Dispatch;
            Status = Disp->WskBind(Sock->WskSocket, (PSOCKADDR)&Sock->LocalAddress, 0, Irp);
        }
    } else {
        const WSK_PROVIDER_DATAGRAM_DISPATCH* Disp = (const WSK_PROVIDER_DATAGRAM_DISPATCH*)Sock->WskSocket->Dispatch;
        Status = Disp->WskBind(Sock->WskSocket, (PSOCKADDR)&Sock->LocalAddress, 0, Irp);
    }

    Status = KsWaitIrp(Irp, &Ctx);
    return Status;
}

/* =========================================================================
 * РЕАЛИЗАЦИЯ ПУБЛИЧНОГО API
 * ========================================================================= */

/**
 * @brief Реализация функции ksSocket для платформы Windows.
 * @details Выделяет память под структуру `KS_SOCKET` из невыгружаемого пула (`NonPagedPool`).
 */
KS_SOCKET* ksSocket(int Protocol) {
    KS_SOCKET* Sock;
    if (!g_WskReady || KeGetCurrentIrql() > PASSIVE_LEVEL) return NULL;

    Sock = (KS_SOCKET*)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(KS_SOCKET), 'ksKS');
    if (!Sock) return NULL;

    Sock->Protocol    = Protocol;
    Sock->WskSocket   = NULL;
    Sock->IsBound     = FALSE;
    Sock->IsListening = FALSE;
    return Sock;
}

/**
 * @brief Реализация функции ksBind для платформы Windows.
 * @details Запоминает локальный IP и порт для последующей ленивой инициализации в WSK.
 */
int ksBind(KS_SOCKET* Sock, const char* Ip, unsigned short Port) {
    if (!Sock || !Ip || KeGetCurrentIrql() > PASSIVE_LEVEL) return KS_ERR_INVAL;
    if (Sock->WskSocket != NULL) return KS_ERR; /* Уже инициализирован WSK */

    RtlZeroMemory(&Sock->LocalAddress, sizeof(Sock->LocalAddress));
    if (!NT_SUCCESS(KsStringToSockaddr(Ip, Port, &Sock->LocalAddress))) return KS_ERR_INVAL;

    Sock->IsBound = TRUE;
    return KS_OK;
}

/**
 * @brief Реализация функции ksListen для платформы Windows.
 * @details Запускает ленивую инициализацию сокета с флагом `WSK_FLAG_LISTEN_SOCKET`.
 */
int ksListen(KS_SOCKET* Sock, int Backlog) {
    NTSTATUS Status;
    UNREFERENCED_PARAMETER(Backlog); /* В WSK размер очереди не задается явно */

    if (!Sock || Sock->Protocol != KS_TCP || KeGetCurrentIrql() > PASSIVE_LEVEL) return KS_ERR_INVAL;

    Status = KsLazyCreateWskSocket(Sock, WSK_FLAG_LISTEN_SOCKET);
    if (!NT_SUCCESS(Status)) return KS_ERR;

    Sock->IsListening = TRUE;
    return KS_OK;
}

/**
 * @brief Реализация функции ksAccept для платформы Windows.
 * @details Выполняет блокирующий IRP-запрос к `WskAccept`.
 */
int ksAccept(KS_SOCKET* Sock, KS_SOCKET** NewSock, char* ClientIp, unsigned short* ClientPort) {
    const WSK_PROVIDER_LISTEN_DISPATCH* ListenDisp;
    KS_IRP_CTX_EX CtxEx;
    PIRP          Irp;
    SOCKADDR_IN   RemoteAddr;
    NTSTATUS      Status;

    if (!Sock || !NewSock || !Sock->IsListening || KeGetCurrentIrql() > PASSIVE_LEVEL) return KS_ERR_INVAL;

    ListenDisp = (const WSK_PROVIDER_LISTEN_DISPATCH*)Sock->WskSocket->Dispatch;
    Irp = KsAllocateIrpEx(&CtxEx);
    if (!Irp) return KS_ERR_NOMEM;

    RtlZeroMemory(&RemoteAddr, sizeof(RemoteAddr));

    Status = ListenDisp->WskAccept(Sock->WskSocket, 0, NULL, NULL, NULL, (PSOCKADDR)&RemoteAddr, Irp);
    KeWaitForSingleObject(&CtxEx.Base.Event, Executive, KernelMode, FALSE, NULL);
    IoFreeIrp(Irp);

    if (!NT_SUCCESS(CtxEx.Base.Status)) return KS_ERR;

    *NewSock = ksSocket(KS_TCP);
    if (!*NewSock) {
        /* Придется закрыть принятый сокет, если не хватило памяти под структуру */
        const WSK_PROVIDER_BASIC_DISPATCH* BasicDisp = (const WSK_PROVIDER_BASIC_DISPATCH*)((PWSK_SOCKET)CtxEx.Information)->Dispatch;
        KS_IRP_CTX Ctx;
        Irp = KsAllocateIrp(&Ctx);
        if (Irp) { BasicDisp->WskCloseSocket((PWSK_SOCKET)CtxEx.Information, Irp); KsWaitIrp(Irp, &Ctx); }
        return KS_ERR_NOMEM;
    }

    (*NewSock)->WskSocket = (PWSK_SOCKET)CtxEx.Information;
    (*NewSock)->IsBound = TRUE; 
    
    KsSockaddrToString(&RemoteAddr, ClientIp, ClientPort);
    return KS_OK;
}

/**
 * @brief Реализация функции ksConnect для платформы Windows.
 * @details Запускает ленивую инициализацию сокета с флагом `WSK_FLAG_CONNECTION_SOCKET`.
 */
int ksConnect(KS_SOCKET* Sock, const char* Ip, unsigned short Port) {
    const WSK_PROVIDER_CONNECTION_DISPATCH* ConnDisp;
    SOCKADDR_IN RemoteAddr;
    KS_IRP_CTX  Ctx;
    PIRP        Irp;
    NTSTATUS    Status;

    if (!Sock || !Ip || Port == 0 || Sock->Protocol != KS_TCP || KeGetCurrentIrql() > PASSIVE_LEVEL) return KS_ERR_INVAL;

    if (!NT_SUCCESS(KsStringToSockaddr(Ip, Port, &RemoteAddr))) return KS_ERR_INVAL;

    Status = KsLazyCreateWskSocket(Sock, WSK_FLAG_CONNECTION_SOCKET);
    if (!NT_SUCCESS(Status)) return KS_ERR;

    ConnDisp = (const WSK_PROVIDER_CONNECTION_DISPATCH*)Sock->WskSocket->Dispatch;
    Irp = KsAllocateIrp(&Ctx);
    if (!Irp) return KS_ERR_NOMEM;

    Status = ConnDisp->WskConnect(Sock->WskSocket, (PSOCKADDR)&RemoteAddr, 0, Irp);
    Status = KsWaitIrp(Irp, &Ctx);

    if (!NT_SUCCESS(Status)) return (Status == STATUS_CONNECTION_REFUSED) ? KS_ERR_CONN : KS_ERR;
    return KS_OK;
}

/**
 * @brief Реализация функции ksSend для платформы Windows.
 * @details Инкапсулирует буфер в MDL (Memory Descriptor List) для безопасной передачи WSK.
 */
int ksSend(KS_SOCKET* Sock, const void* Buf, unsigned int Len) {
    const WSK_PROVIDER_CONNECTION_DISPATCH* ConnDisp;
    WSK_BUF    WskBuf;
    PMDL       Mdl;
    KS_IRP_CTX Ctx;
    PIRP       Irp;
    NTSTATUS   Status;

    if (!Sock || !Buf || Len == 0 || KeGetCurrentIrql() > PASSIVE_LEVEL) return KS_ERR_INVAL;

    Mdl = IoAllocateMdl((PVOID)Buf, Len, FALSE, FALSE, NULL);
    if (!Mdl) return KS_ERR_NOMEM;
    MmBuildMdlForNonPagedPool(Mdl);

    WskBuf.Mdl = Mdl; WskBuf.Offset = 0; WskBuf.Length = Len;

    ConnDisp = (const WSK_PROVIDER_CONNECTION_DISPATCH*)Sock->WskSocket->Dispatch;
    Irp = KsAllocateIrp(&Ctx);
    if (!Irp) { IoFreeMdl(Mdl); return KS_ERR_NOMEM; }

    Status = ConnDisp->WskSend(Sock->WskSocket, &WskBuf, 0, Irp);
    Status = KsWaitIrp(Irp, &Ctx);
    IoFreeMdl(Mdl);

    return NT_SUCCESS(Status) ? (int)Len : KS_ERR;
}

/**
 * @brief Реализация функции ksRecv для платформы Windows.
 * @details Читает количество реально полученных байт из `IoStatus.Information`.
 */
int ksRecv(KS_SOCKET* Sock, void* Buf, unsigned int Len) {
    const WSK_PROVIDER_CONNECTION_DISPATCH* ConnDisp;
    WSK_BUF       WskBuf;
    PMDL          Mdl;
    KS_IRP_CTX_EX Ctx;
    PIRP          Irp;

    if (!Sock || !Buf || Len == 0 || KeGetCurrentIrql() > PASSIVE_LEVEL) return KS_ERR_INVAL;

    Mdl = IoAllocateMdl(Buf, Len, FALSE, FALSE, NULL);
    if (!Mdl) return KS_ERR_NOMEM;
    MmBuildMdlForNonPagedPool(Mdl);

    WskBuf.Mdl = Mdl; WskBuf.Offset = 0; WskBuf.Length = Len;

    ConnDisp = (const WSK_PROVIDER_CONNECTION_DISPATCH*)Sock->WskSocket->Dispatch;
    Irp = KsAllocateIrpEx(&Ctx);
    if (!Irp) { IoFreeMdl(Mdl); return KS_ERR_NOMEM; }

    ConnDisp->WskReceive(Sock->WskSocket, &WskBuf, 0, Irp);
    KeWaitForSingleObject(&Ctx.Base.Event, Executive, KernelMode, FALSE, NULL);
    IoFreeIrp(Irp); IoFreeMdl(Mdl);

    if (Ctx.Base.Status == STATUS_CONNECTION_DISCONNECTED || Ctx.Base.Status == STATUS_CONNECTION_RESET) return 0;
    return NT_SUCCESS(Ctx.Base.Status) ? (int)Ctx.Information : KS_ERR;
}

/**
 * @brief Реализация функции ksSendTo для платформы Windows.
 * @details Инициализирует датаграммный сокет при первом вызове, формирует MDL и отправляет пакет.
 */
int ksSendTo(KS_SOCKET* Sock, const void* Buf, unsigned int Len, const char* Ip, unsigned short Port) {
    const WSK_PROVIDER_DATAGRAM_DISPATCH* DgramDisp;
    SOCKADDR_IN RemoteAddr;
    WSK_BUF     WskBuf;
    PMDL        Mdl;
    KS_IRP_CTX  Ctx;
    PIRP        Irp;
    NTSTATUS    Status;

    if (!Sock || !Buf || Len == 0 || !Ip || KeGetCurrentIrql() > PASSIVE_LEVEL) return KS_ERR_INVAL;
    if (!NT_SUCCESS(KsStringToSockaddr(Ip, Port, &RemoteAddr))) return KS_ERR_INVAL;

    Status = KsLazyCreateWskSocket(Sock, WSK_FLAG_DATAGRAM_SOCKET);
    if (!NT_SUCCESS(Status)) return KS_ERR;

    Mdl = IoAllocateMdl((PVOID)Buf, Len, FALSE, FALSE, NULL);
    if (!Mdl) return KS_ERR_NOMEM;
    MmBuildMdlForNonPagedPool(Mdl);

    WskBuf.Mdl = Mdl; WskBuf.Offset = 0; WskBuf.Length = Len;

    DgramDisp = (const WSK_PROVIDER_DATAGRAM_DISPATCH*)Sock->WskSocket->Dispatch;
    Irp = KsAllocateIrp(&Ctx);
    if (!Irp) { IoFreeMdl(Mdl); return KS_ERR_NOMEM; }

    Status = DgramDisp->WskSendTo(Sock->WskSocket, &WskBuf, 0, (PSOCKADDR)&RemoteAddr, 0, NULL, Irp);
    Status = KsWaitIrp(Irp, &Ctx);
    IoFreeMdl(Mdl);

    return NT_SUCCESS(Status) ? (int)Len : KS_ERR;
}

/**
 * @brief Реализация функции ksRecvFrom для платформы Windows.
 * @details Парсит бинарный адрес `SOCKADDR_IN`, заполненный подсистемой WSK, в строковый IP и порт.
 */
int ksRecvFrom(KS_SOCKET* Sock, void* Buf, unsigned int Len, char* SrcIp, unsigned short* SrcPort) {
    const WSK_PROVIDER_DATAGRAM_DISPATCH* DgramDisp;
    WSK_BUF       WskBuf;
    PMDL          Mdl;
    KS_IRP_CTX_EX Ctx;
    PIRP          Irp;
    SOCKADDR_IN   FromAddr;
    NTSTATUS      Status;

    if (!Sock || !Buf || Len == 0 || KeGetCurrentIrql() > PASSIVE_LEVEL) return KS_ERR_INVAL;

    Status = KsLazyCreateWskSocket(Sock, WSK_FLAG_DATAGRAM_SOCKET);
    if (!NT_SUCCESS(Status)) return KS_ERR;

    Mdl = IoAllocateMdl(Buf, Len, FALSE, FALSE, NULL);
    if (!Mdl) return KS_ERR_NOMEM;
    MmBuildMdlForNonPagedPool(Mdl);

    WskBuf.Mdl = Mdl; WskBuf.Offset = 0; WskBuf.Length = Len;
    RtlZeroMemory(&FromAddr, sizeof(FromAddr));

    DgramDisp = (const WSK_PROVIDER_DATAGRAM_DISPATCH*)Sock->WskSocket->Dispatch;
    Irp = KsAllocateIrpEx(&Ctx);
    if (!Irp) { IoFreeMdl(Mdl); return KS_ERR_NOMEM; }

    DgramDisp->WskReceiveFrom(Sock->WskSocket, &WskBuf, 0, (PSOCKADDR)&FromAddr, 0, NULL, NULL, Irp);
    KeWaitForSingleObject(&Ctx.Base.Event, Executive, KernelMode, FALSE, NULL);
    IoFreeIrp(Irp); IoFreeMdl(Mdl);

    if (!NT_SUCCESS(Ctx.Base.Status)) return KS_ERR;

    KsSockaddrToString(&FromAddr, SrcIp, SrcPort);
    return (int)Ctx.Information;
}

/**
 * @brief Реализация функции ksClose для платформы Windows.
 * @details Корректно завершает IRP закрытия сокета и освобождает память пула.
 */
void ksClose(KS_SOCKET* Sock) {
    if (!Sock) return;
    if (KeGetCurrentIrql() > PASSIVE_LEVEL) return;

    if (Sock->WskSocket) {
        const WSK_PROVIDER_BASIC_DISPATCH* BasicDisp = (const WSK_PROVIDER_BASIC_DISPATCH*)Sock->WskSocket->Dispatch;
        KS_IRP_CTX Ctx;
        PIRP Irp = KsAllocateIrp(&Ctx);
        if (Irp) {
            BasicDisp->WskCloseSocket(Sock->WskSocket, Irp);
            KsWaitIrp(Irp, &Ctx);
        }
    }
    ExFreePoolWithTag(Sock, 'ksKS');
}