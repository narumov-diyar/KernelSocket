/**
 * @file ks_windows_internal.h
 * @brief Внутренние типы и функции Windows-реализации KernelSocket.
 *
 * @details
 * Файл используется только внутри backend'а `src/windows/` и не является
 * частью публичного API. Здесь описана структура непрозрачного дескриптора
 * `KS_SOCKET`, а также объявлены функции инициализации подсистемы WSK.
 */

#ifndef KS_WINDOWS_INTERNAL_H
#define KS_WINDOWS_INTERNAL_H

#include <ntddk.h>
#include <wsk.h>
#include "../../include/ks_api.h"

/* =========================================================================
 * Внутренняя структура сокета (Реализация непрозрачного типа KS_SOCKET)
 * ========================================================================= */

/**
 * @struct KS_SOCKET
 * @brief Внутреннее представление сетевого сокета для ОС Windows.
 * 
 * @details Данная структура скрыта от пользователя API посредством паттерна Opaque Pointer.
 * Она хранит состояние сокета до момента его фактической аллокации в подсистеме WSK 
 * (Ленивая инициализация).
 */
struct KS_SOCKET {
    int         Protocol;      /**< Транспортный протокол (KS_TCP или KS_UDP). */
    PWSK_SOCKET WskSocket;     /**< Указатель на ядерный объект WSK (NULL до вызова ksConnect/ksListen/ksSendTo). */
    
    BOOLEAN     IsBound;       /**< Флаг, указывающий, был ли вызван ksBind для данного сокета. */
    SOCKADDR_IN LocalAddress;  /**< Сохраненный локальный адрес для отложенной привязки. */
    BOOLEAN     IsListening;   /**< Флаг, указывающий, переведен ли TCP-сокет в режим прослушивания (ksListen). */
};

/* =========================================================================
 * Инициализация WSK (вызывается из DriverEntry / DriverUnload)
 * ========================================================================= */

/**
 * @brief Инициализирует регистрацию клиента WSK и захватывает Provider NPI.
 *
 * @details Функция должна быть вызвана строго один раз (например, в DriverEntry) 
 * перед использованием любых других функций API KernelSocket.
 * 
 * @warning Вызов на IRQL >= DISPATCH_LEVEL приведет к BSOD.
 *
 * @return `STATUS_SUCCESS` при успешной инициализации, иначе код ошибки NTSTATUS.
 */
NTSTATUS KsWskInit(void);

/**
 * @brief Освобождает Provider NPI и снимает регистрацию клиента WSK.
 * 
 * @details Функция должна быть вызвана при выгрузке драйвера (в DriverUnload).
 * Она блокирует выполнение до тех пор, пока все незавершенные IRP-запросы WSK 
 * не будут обработаны.
 * 
 * @warning Вызов на IRQL >= DISPATCH_LEVEL приведет к BSOD.
 */
void     KsWskCleanup(void);

#endif /* KS_WINDOWS_INTERNAL_H */