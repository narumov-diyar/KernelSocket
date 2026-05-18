"""!
@file user_test.py
@brief User-mode утилита для тестирования проекта KernelSocket.

@details
Скрипт предоставляет ручные сценарии проверки TCP/UDP взаимодействия с
ядром (Windows/Linux): режим сервера, одиночные клиентские прогоны и
интерактивный режим. Используется в тестовом контуре команды Kernel Crew.

Запуск: `python user_test.py`
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
	"""!
	@brief Возвращает текущее время.
	@return Строка с текущим локальным временем в формате ЧЧ:ММ:СС.
	"""
	return datetime.now().strftime("%H:%M:%S")


def log(tag: str, message: str):
	"""!
	@brief Единый формат вывода логов для консоли.
	
	@param tag     Тег подсистемы или потока (например, 'TCP-СЕРВЕР').
	@param message Текст логируемого сообщения.
	"""
	print(f"[{get_time()}] [{tag}] {message}")


def load_config():
	"""!
	@brief Загружает сетевые настройки из конфигурационного JSON-файла.
	
	@details Если файл (ks_config.txt) существует, его содержимое обновляет
	глобальный словарь `config`. При ошибках (файл не найден или поврежден) 
	используются настройки по умолчанию.
	"""
	global config
	if os.path.exists(CONFIG_FILE):
		try:
			with open(CONFIG_FILE, "r", encoding="utf-8") as f:
				config.update(json.load(f))
		except Exception:
			pass


def save_config():
	"""!
	@brief Сохраняет текущие сетевые настройки в JSON-файл.
	"""
	try:
		with open(CONFIG_FILE, "w", encoding="utf-8") as f:
			json.dump(config, f, indent=4)
	except Exception:
		pass


# =========================================================================
# РЕЖИМ СЕРВЕРА (Прием данных из Ядра)
# =========================================================================
def run_tcp_server():
	"""!
	@brief Системный поток TCP-сервера.
	
	@details Принимает входящие TCP-подключения, читает полученные данные и 
	отправляет клиенту квитанцию (ACK). Обрабатывает таймауты (`s.settimeout`) 
	для безопасной остановки потока при изменении флага `server_running`.
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
	"""!
	@brief Системный поток UDP-сервера.
	
	@details Работает в режиме Fire-and-Forget: непрерывно ожидает датаграммы 
	от клиентов и выводит их в консоль без отправки подтверждения. Поддерживает 
	безопасную остановку через таймауты.
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
	"""!
	@brief Запускает одиночные тестовые сценарии для TCP и UDP.
	
	@details Устанавливает соединения с целевым узлом, отправляет по одному 
	пакету и ожидает ответ от ядерного сервера (если предусмотрено протоколом).
	"""
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
	"""!
	@brief Запускает режим интерактивного консольного чата.
	
	@details Позволяет пользователю отправлять произвольные текстовые 
	сообщения на ядерный сервер по выбранному протоколу в реальном времени.
	"""
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
	"""!
	@brief Меню настройки целевого IP-адреса и портов.
	
	@details Запрашивает у пользователя новые сетевые параметры, 
	обновляет конфигурацию в памяти и записывает её в файл через `save_config()`.
	"""
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
	"""!
	@brief Точка входа в программу.
	
	@details Загружает настройки, подготавливает консоль (кодировка CP1251
	для ОС Windows) и отрисовывает главное интерактивное меню утилиты.
	"""
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