"""
KernelSocket User-Mode Testing Tool
===================================
Утилита для тестирования сетевого взаимодействия с модулями режима ядра (Windows/Linux).

Функционал:
- TCP Сервер и Клиент (Надежный обмен данными с квитанциями).
- UDP Сервер и Клиент (Режим стриминга / Fire-and-Forget).
- Автоматическое кодирование/декодирование в CP1251 (для совместимости с ядром Windows).
- Сохранение настроек IP и портов в конфигурационный файл.

Запуск: python user_test.py
"""

import socket
import threading
import json
import os
import sys
from datetime import datetime

# =========================================================================
# [НАСТРОЙКИ ПО УМОЛЧАНИЮ И ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ]
# =========================================================================
CONFIG_FILE = "ks_config.txt"

config = {
    "ip": "127.0.0.1",
    "tcp_port": 9000,
    "udp_port": 9001
}

# Флаг для безопасной остановки серверных потоков
server_running = False


def get_time() -> str:
    """Возвращает текущее время в формате ЧЧ:ММ:СС."""
    return datetime.now().strftime("%H:%M:%S")


def log(tag: str, message: str):
    """
    Единый формат вывода логов.
    
    :param tag: Тег подсистемы (например, 'TCP-СЕРВЕР')
    :param message: Текст сообщения
    """
    print(f"[{get_time()}] [{tag}] {message}")


def load_config():
    """Загружает сетевые настройки из JSON-файла, если он существует."""
    global config
    if os.path.exists(CONFIG_FILE):
        try:
            with open(CONFIG_FILE, "r", encoding="utf-8") as f:
                config.update(json.load(f))
        except Exception:
            pass


def save_config():
    """Сохраняет текущие сетевые настройки в JSON-файл."""
    try:
        with open(CONFIG_FILE, "w", encoding="utf-8") as f:
            json.dump(config, f, indent=4)
    except Exception:
        pass


# =========================================================================
# РЕЖИМ СЕРВЕРА (Прием данных из Ядра)
# =========================================================================
def run_tcp_server():
    """
    Поток TCP-сервера. 
    Принимает соединения, читает данные и отправляет квитанцию (ACK).
    """
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        s.bind(('0.0.0.0', config["tcp_port"]))
        s.listen(5)
        s.settimeout(1.0) # Таймаут 1 сек для проверки флага server_running
        
        log("TCP-СЕРВЕР", f"Ожидание подключений на порту {config['tcp_port']}...")
        
        while server_running:
            try:
                conn, addr = s.accept()
            except socket.timeout:
                continue
            except Exception:
                break

            data = conn.recv(2048)
            if data:
                # Декодируем из CP1251, т.к. DbgPrint в Windows работает с ней
                text = data.decode('cp1251', errors='ignore')
                log("TCP-СЕРВЕР", f"Подключение от {addr[0]}:{addr[1]}")
                log("TCP-СЕРВЕР", f"Получено: '{text}'")
                
                # Отправка квитанции обратно в ядро (НА АНГЛИЙСКОМ для совместимости с Linux)
                reply = f"ACK [Time: {get_time()} | Bytes received: {len(data)}]"
                conn.send(reply.encode('cp1251', errors='ignore'))
                log("TCP-СЕРВЕР", f"Отправлена квитанция: {reply}\n")
                
            conn.close()
    except Exception as e:
        if server_running: 
            log("TCP-ОШИБКА", str(e))
    finally:
        s.close()


def run_udp_server():
    """
    Поток UDP-сервера. 
    Работает в режиме Fire-and-Forget: только принимает и логирует датаграммы.
    """
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        s.bind(('0.0.0.0', config["udp_port"]))
        s.settimeout(1.0)
        
        log("UDP-СЕРВЕР", f"Ожидание датаграмм на порту {config['udp_port']}...")
        
        while server_running:
            try:
                data, addr = s.recvfrom(2048)
            except socket.timeout:
                continue
            except Exception:
                break

            if data:
                text = data.decode('cp1251', errors='ignore')
                # UDP - это стриминг. Просто принимаем данные без квитанций
                log("UDP-СЕРВЕР", f"Получено от {addr[0]}:{addr[1]}: '{text}'")
                
    except Exception as e:
        if server_running: 
            log("UDP-ОШИБКА", str(e))
    finally:
        s.close()


# =========================================================================
# РЕЖИМ КЛИЕНТА (Отправка данных в Ядро)
# =========================================================================
def run_single_tests():
    """Отправляет по одному тестовому пакету для TCP и UDP."""
    print(f"\n--- Отправка тестовых пакетов на {config['ip']} ---")
    
    # TCP (Отправка с ожиданием квитанции)
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(2)
        s.connect((config["ip"], config["tcp_port"]))
        
        # Сообщение НА АНГЛИЙСКОМ
        msg = f"Single TCP test from Python [{get_time()}]".encode('cp1251')
        s.send(msg)
        log("TCP-КЛИЕНТ", f"Отправлено: '{msg.decode('cp1251')}'")
        
        reply = s.recv(1024)
        log("TCP-КЛИЕНТ", f"Ответ узла: '{reply.decode('cp1251', errors='ignore')}'")
        s.close()
    except Exception as e:
        log("TCP-ОШИБКА", f"Сбой подключения: {e}")

    # UDP (Отправка без ожидания ответа - Fire-and-Forget)
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        
        # Сообщение НА АНГЛИЙСКОМ
        msg = f"Single UDP test from Python [{get_time()}]".encode('cp1251')
        s.sendto(msg, (config["ip"], config["udp_port"]))
        log("UDP-КЛИЕНТ", f"Отправлено (Стриминг): '{msg.decode('cp1251')}'")
        s.close()
    except Exception as e:
        log("UDP-ОШИБКА", f"Сбой отправки: {e}")


def run_interactive():
    """Интерактивный чат с ядерным сервером."""
    print("\n--- Интерактивный режим (Чат) ---")
    proto = input("Протокол (1 - TCP, 2 - UDP): ").strip()
    if proto not in ['1', '2']:
        print("[-] Ошибка выбора.")
        return

    tag = "TCP-КЛИЕНТ" if proto == '1' else "UDP-КЛИЕНТ"
    print(f"\nСоединение с {config['ip']}. Введите 'exit' для выхода.")
    
    while True:
        text = input("\nОтправить: ")
        if text.lower() == 'exit':
            break
        if not text:
            continue

        msg = text.encode('cp1251')

        try:
            if proto == '1': 
                s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                s.settimeout(2)
                s.connect((config["ip"], config["tcp_port"]))
                s.send(msg)
                reply = s.recv(1024)
                log(tag, f"Ответ: {reply.decode('cp1251', errors='ignore')}")
                s.close()
            else: 
                s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                s.sendto(msg, (config["ip"], config["udp_port"]))
                log(tag, "Успешно отправлено.")
                s.close()
        except Exception as e:
            log(f"{tag}-ОШИБКА", str(e))


def menu_settings():
    """Меню изменения IP и портов с сохранением в файл."""
    print("\n--- Настройки ---")
    new_ip = input(f"IP-адрес [{config['ip']}]: ").strip()
    if new_ip: 
        config['ip'] = new_ip
    
    new_tcp = input(f"TCP порт [{config['tcp_port']}]: ").strip()
    if new_tcp.isdigit(): 
        config['tcp_port'] = int(new_tcp)
    
    new_udp = input(f"UDP порт [{config['udp_port']}]: ").strip()
    if new_udp.isdigit(): 
        config['udp_port'] = int(new_udp)
    
    save_config()
    print("[+] Настройки сохранены.")


# =========================================================================
# ГЛАВНЫЙ ЦИКЛ
# =========================================================================
def main():
    global server_running
    load_config()
    
    # Настраиваем консоль Windows на корректное отображение кириллицы
    if os.name == 'nt':
        os.system('chcp 1251 >nul 2>&1')

    while True:
        print("\n" + "=" * 46)
        print(" Утилита сетевого тестирования KernelSocket")
        print(f" Узел: {config['ip']} | TCP: {config['tcp_port']} | UDP: {config['udp_port']}")
        print("=" * 46)
        print(" 1. Режим СЕРВЕРА (Прослушивание портов)")
        print(" 2. Режим КЛИЕНТА (Одиночные тестовые пакеты)")
        print(" 3. Режим КЛИЕНТА (Интерактивный чат)")
        print(" 4. Настройки сети")
        print(" 0. Выход")
        
        choice = input("\nВыберите действие: ").strip()

        if choice == '1':
            server_running = True
            t1 = threading.Thread(target=run_tcp_server)
            t2 = threading.Thread(target=run_udp_server)
            
            print("\n[Нажмите ENTER для остановки серверов]\n")
            
            t1.start()
            t2.start()
            
            input() # Блокируем главный поток до нажатия Enter
                  
            server_running = False
            t1.join()
            t2.join()
            print("\n[+] Серверы успешно остановлены. Порты освобождены.")
            
        elif choice == '2':
            run_single_tests()
            
        elif choice == '3':
            run_interactive()
            
        elif choice == '4':
            menu_settings()
            
        elif choice == '0':
            sys.exit(0)
            
        else:
            print("[-] Ошибка ввода. Выберите пункт из меню.")


if __name__ == "__main__":
    # Защита от прерывания скрипта через Ctrl+C
    try:
        main()
    except KeyboardInterrupt:
        print("\n[!] Принудительное завершение работы.")
        server_running = False
        sys.exit(0)