#ifndef KS_LINUX_INTERNAL_H
#define KS_LINUX_INTERNAL_H

#include <linux/net.h>
#include <linux/in.h>
#include "../../include/ks_api.h"

/* =========================================================================
 * Внутренняя структура сокета для Linux
 * ========================================================================= */
struct KS_SOCKET {
    int Protocol;          /**< KS_TCP или KS_UDP */
    struct socket *sock;   /**< Стандартный указатель на сокет ядра Linux */
};

#endif /* KS_LINUX_INTERNAL_H */