# BitTorrent Client

Полнофункциональный BitTorrent клиент на C++17 с поддержкой основных возможностей протокола BitTorrent.

## Возможности

- Парсинг .torrent файлов (Bencode формат)
- Взаимодействие с HTTP трекерами
- BitTorrent peer протокол
- Управление загрузкой по кускам (pieces)
- Многопоточная архитектура
- Поддержка одно- и многофайловых торрентов
- SHA1 верификация загруженных данных
- CLI интерфейс

## Архитектура

Проект разделен на несколько основных компонентов:

### Основные модули:

1. **Bencode Parser** (`bencode.h/cpp`)
   - Парсинг формата Bencode (используется в .torrent файлах)
   - Поддержка integers, strings, lists, dictionaries
   - Encoding/Decoding

2. **Torrent File** (`torrent_file.h/cpp`)
   - Парсинг .torrent файлов
   - Извлечение метаданных (announce URL, размер файлов, info hash)
   - Поддержка single-file и multi-file режимов

3. **Tracker Client** (`tracker_client.h/cpp`)
   - HTTP запросы к трекерам
   - Получение списка пиров
   - Парсинг ответов трекера (compact/dictionary формат)

4. **Peer Connection** (`peer_connection.h/cpp`)
   - Реализация BitTorrent peer протокола
   - Handshake с пирами
   - Обмен сообщениями (choke, interested, request, piece, etc.)

5. **Piece Manager** (`piece_manager.h/cpp`)
   - Управление загрузкой кусков файла
   - Верификация кусков через SHA1
   - Битфилд отслеживания загруженных кусков
   - Стратегии загрузки

6. **File Manager** (`file_manager.h/cpp`)
   - Работа с файловой системой
   - Чтение/запись кусков в файлы
   - Поддержка многофайловых торрентов

7. **Download Manager** (`download_manager.h/cpp`)
   - Координация всего процесса загрузки
   - Управление потоками
   - Статистика (скорость, прогресс)

8. **Utils** (`utils.h/cpp`)
   - SHA1 хеширование (OpenSSL)
   - URL encoding
   - Hex конвертация
   - Генерация Peer ID
   - Форматирование вывода

## Требования

- C++17 или выше
- CMake 3.15+
- OpenSSL (для SHA1)
- Компилятор: GCC, Clang, MSVC

### Windows:
- Visual Studio 2019+ или MinGW
- OpenSSL (можно установить через vcpkg)

### Linux/macOS:
- GCC 7+ или Clang 5+
- OpenSSL (обычно предустановлен)

## Сборка

### Windows (Visual Studio):

```bash
# Установка OpenSSL через vcpkg (опционально)
vcpkg install openssl

# Сборка
mkdir build
cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=[путь к vcpkg]/scripts/buildsystems/vcpkg.cmake
cmake --build . --config Release
```

### Windows (MinGW):

```bash
mkdir build
cd build
cmake .. -G "MinGW Makefiles"
cmake --build .
```

### Linux/macOS:

```bash
# Установка зависимостей (Ubuntu/Debian)
sudo apt-get install build-essential cmake libssl-dev

# Сборка
mkdir build
cd build
cmake ..
make -j$(nproc)
```

## Использование

```bash
# Базовое использование
./torrent_client example.torrent

# С указанием директории загрузки
./torrent_client example.torrent ./my_downloads
```

### Опции:
- `<torrent_file>` - Путь к .torrent файлу (обязательно)
- `[download_dir]` - Директория для сохранения файлов (по умолчанию: ./downloads)

## Структура проекта

```
filename/
├── CMakeLists.txt          # Конфигурация сборки
├── README.md               # Документация
├── .gitignore
├── include/                # Заголовочные файлы
│   ├── bencode.h
│   ├── torrent_file.h
│   ├── tracker_client.h
│   ├── peer_connection.h
│   ├── piece_manager.h
│   ├── file_manager.h
│   ├── download_manager.h
│   └── utils.h
├── src/                    # Исходные файлы
│   ├── main.cpp
│   ├── bencode.cpp
│   ├── utils.cpp
│   ├── torrent_file.cpp
│   ├── tracker_client.cpp
│   ├── peer_connection.cpp
│   ├── piece_manager.cpp
│   ├── file_manager.cpp
│   └── download_manager.cpp
├── downloads/              # Директория загрузок
├── examples/               # Примеры .torrent файлов
└── tests/                  # Юнит-тесты (TODO)
```

## BitTorrent Protocol

### Bencode формат:
- **Integers**: `i<number>e` → `i42e` = 42
- **Strings**: `<length>:<data>` → `4:spam` = "spam"
- **Lists**: `l<items>e` → `l4:spam4:eggse` = ["spam", "eggs"]
- **Dictionaries**: `d<key><value>...e` → `d3:cow3:mooe` = {"cow": "moo"}

### Peer протокол:
- Handshake: `<pstrlen><pstr><reserved><info_hash><peer_id>`
- Сообщения: `<length><id><payload>`
- Типы сообщений: choke, unchoke, interested, have, bitfield, request, piece, cancel

## Текущий статус

### Реализовано:
- [x] Bencode parser
- [x] Torrent file parser
- [x] Tracker client (структура)
- [x] Peer connection (структура)
- [x] Piece manager
- [x] File manager
- [x] Download manager (базовая координация)
- [x] Многопоточность
- [x] CLI интерфейс

### TODO (для полной функциональности):
- [ ] HTTP клиент для трекеров (сейчас stub)
- [ ] TCP сокеты для peer connections
- [ ] Полная реализация peer протокола
- [ ] UDP трекеры
- [ ] DHT (Distributed Hash Table)
- [ ] Магнет-ссылки
- [ ] uTP (micro Transport Protocol)
- [ ] Encryption
- [ ] Resume capability (возобновление загрузки)
- [ ] Web UI
- [ ] Юнит-тесты

## Дальнейшее развитие

### Приоритетные задачи:
1. **HTTP клиент** - Добавить libcurl или реализовать простой HTTP клиент
2. **TCP сокеты** - Реализовать подключение к пирам
3. **Peer loop** - Полный цикл обмена данными с пирами
4. **Тестирование** - Добавить юнит-тесты для всех компонентов

### Продвинутые возможности:
- Оптимизация: piece picking strategies (rarest-first, random-first)
- Bandwidth management (ограничение скорости)
- Port forwarding (UPnP/NAT-PMP)
- IPv6 поддержка
- Web seeds
- Super-seeding режим

## Лицензия

MIT License - свободное использование в любых целях.

## Ссылки

- [BitTorrent Protocol Specification](http://www.bittorrent.org/beps/bep_0003.html)
- [Bencode Format](https://en.wikipedia.org/wiki/Bencode)
- [BitTorrent Enhancement Proposals (BEPs)](http://www.bittorrent.org/beps/bep_0000.html)

## Автор

Пет-проект для изучения C++ и сетевых протоколов.
