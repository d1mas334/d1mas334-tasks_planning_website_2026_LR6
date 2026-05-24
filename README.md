# Лабораторная работа 05. Оптимизация производительности через кеширование и rate limiting

## Дисциплина

Архитектура информационных систем / Программная инженерия.

## Вариант

Вариант 10 - планирование задач.

Приложение содержит основные сущности:

- пользователь / исполнитель;
- цель;
- задача.

ЛР5 развивает результат ЛР4: REST API на C++20 / Yandex Userver продолжает использовать PostgreSQL 16 для `users`, `goals`, `tasks` и MongoDB 7 для `task_activity`, `task_comments`, `notification_log`. Новое в ЛР5: Redis 7, Cache-Aside кеширование горячих read endpoints и fixed-window rate limiting.

## Технологии

- C++20;
- Yandex Userver;
- PostgreSQL 16;
- MongoDB 7;
- Redis 7;
- Docker Compose;
- REST API.

## Структура проекта

```text
.
├── CMakeLists.txt
├── Dockerfile
├── docker-compose.yaml
├── configs/
│   ├── static_config.yaml
│   └── secdist.json
├── db/
│   ├── schema.sql
│   ├── data.sql
│   └── queries.sql
├── mongo/
│   ├── validation.js
│   ├── data.js
│   └── queries.js
├── src/
│   └── main.cpp
├── tests/
│   └── curl_examples.md
├── schema_design.md
├── optimization.md
├── performance_design.md
└── openapi.yaml
```

## Хранилища данных

PostgreSQL хранит нормализованные основные сущности:

- `users` - пользователи, роли и данные авторизации;
- `goals` - цели;
- `tasks` - задачи цели, исполнитель, автор, статус и срок.

MongoDB использует базу `task_planning_mongo` и коллекции:

- `task_activity` - события истории задачи;
- `task_comments` - комментарии к задаче;
- `notification_log` - журнал уведомлений.

Redis используется как инфраструктурный слой:

- `goals:all` - кеш ответа `GET /api/goals`, TTL 60 секунд;
- `goal:{goalId}:tasks` - кеш ответа `GET /api/goals/{goalId}/tasks`, TTL 30 секунд;
- `rate:user-search:{userId}:{windowStartUnixMinute}` - счетчик rate limiting для поиска пользователей.

Подробное проектирование кеширования, rate limiting, hot paths, slow operations и метрик описано в [performance_design.md](performance_design.md).

## Запуск

Собрать и запустить API, PostgreSQL, MongoDB и Redis:

```bash
docker compose up --build
```

API доступен по адресу:

```text
http://localhost:8080
```

Если порт `8080` занят, можно выбрать другой host-порт:

```bash
API_PORT=18080 docker compose up --build
```

В PowerShell:

```powershell
$env:API_PORT = "18080"
docker compose up --build
```

В README основной адрес остается `http://localhost:8080`.

## API endpoints

| Метод | URL | Назначение | Auth |
|---|---|---|---|
| GET | `/ping` | Проверка сервиса | Нет |
| POST | `/api/users` | Создание пользователя | Нет |
| POST | `/api/auth/login` | Получение bearer token | Нет |
| GET | `/api/users/by-login?login=alexey` | Поиск пользователя по логину | Да |
| GET | `/api/users/search?mask=iv` | Поиск пользователей по имени/фамилии, rate limited | Да |
| POST | `/api/goals` | Создание цели, инвалидирует `goals:all` | Да |
| GET | `/api/goals` | Получение целей, `X-Cache: HIT/MISS` | Да |
| POST | `/api/goals/{goalId}/tasks` | Создание задачи, инвалидирует `goal:{goalId}:tasks` | Да |
| GET | `/api/goals/{goalId}/tasks` | Получение задач цели, `X-Cache: HIT/MISS` | Да |
| PATCH | `/api/goals/{goalId}/tasks/{taskId}/status` | Изменение статуса, инвалидирует `goal:{goalId}:tasks` | Да |
| POST | `/api/goals/{goalId}/tasks/{taskId}/comments` | Добавить комментарий к задаче | Да |
| GET | `/api/goals/{goalId}/tasks/{taskId}/comments` | Получить комментарии задачи из MongoDB | Да |
| GET | `/api/goals/{goalId}/tasks/{taskId}/activity` | Получить историю активности задачи из MongoDB | Да |

Учебная авторизация сохранена: после login API возвращает bearer token вида `token-{userId}`.

## Проверка API

Проверить сервис:

```bash
curl http://localhost:8080/ping
```

Получить токен seed-пользователя `alexey/pass123`:

```bash
TOKEN=$(curl -s -X POST http://localhost:8080/api/auth/login \
  -H "Content-Type: application/json" \
  -d '{"login":"alexey","password":"pass123"}' | sed -E 's/.*"token":"([^"]+)".*/\1/')
```

В PowerShell:

```powershell
$login = curl.exe -s -X POST http://localhost:8080/api/auth/login `
  -H "Content-Type: application/json" `
  -d '{\"login\":\"alexey\",\"password\":\"pass123\"}' | ConvertFrom-Json
$env:TOKEN = $login.token
```

Проверить кеш `GET /api/goals`: первый запрос должен вернуть `X-Cache: MISS`, второй - `X-Cache: HIT`.

```bash
curl -i http://localhost:8080/api/goals \
  -H "Authorization: Bearer $TOKEN"

curl -i http://localhost:8080/api/goals \
  -H "Authorization: Bearer $TOKEN"
```

Проверить кеш `GET /api/goals/1/tasks`: первый запрос должен вернуть `X-Cache: MISS`, второй - `X-Cache: HIT`.

```bash
curl -i http://localhost:8080/api/goals/1/tasks \
  -H "Authorization: Bearer $TOKEN"

curl -i http://localhost:8080/api/goals/1/tasks \
  -H "Authorization: Bearer $TOKEN"
```

Проверить инвалидацию `goals:all` после создания цели:

```bash
curl -i -X POST http://localhost:8080/api/goals \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer $TOKEN" \
  -d '{"title":"Cache invalidation check","description":"Goal created to invalidate goals:all"}'

curl -i http://localhost:8080/api/goals \
  -H "Authorization: Bearer $TOKEN"
```

После `POST /api/goals` следующий `GET /api/goals` должен снова вернуть `X-Cache: MISS`.

Проверить инвалидацию `goal:1:tasks` после создания задачи:

```bash
curl -i -X POST http://localhost:8080/api/goals/1/tasks \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer $TOKEN" \
  -d '{"title":"Cache invalidation task","description":"Task created to invalidate goal:1:tasks","assigneeId":2,"dueDate":"2026-06-30"}'

curl -i http://localhost:8080/api/goals/1/tasks \
  -H "Authorization: Bearer $TOKEN"
```

После `POST /api/goals/1/tasks` следующий `GET /api/goals/1/tasks` должен снова вернуть `X-Cache: MISS`.

Проверить инвалидацию `goal:1:tasks` после изменения статуса:

```bash
curl -i -X PATCH http://localhost:8080/api/goals/1/tasks/1/status \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer $TOKEN" \
  -d '{"status":"in_progress"}'

curl -i http://localhost:8080/api/goals/1/tasks \
  -H "Authorization: Bearer $TOKEN"
```

## Rate limiting

`GET /api/users/search?mask=...` ограничен 60 запросами в минуту на авторизованного пользователя. Каждый ответ endpoint добавляет заголовки:

- `X-RateLimit-Limit`;
- `X-RateLimit-Remaining`;
- `X-RateLimit-Reset`.

При превышении лимита API возвращает:

```http
HTTP/1.1 429 Too Many Requests
```

```json
{"error":"rate limit exceeded"}
```

Тест превышения лимита:

```bash
for i in $(seq 1 65); do
  curl -i -s http://localhost:8080/api/users/search?mask=iv \
    -H "Authorization: Bearer $TOKEN" | grep -E "HTTP/|X-RateLimit|rate limit"
done
```

В PowerShell:

```powershell
1..65 | ForEach-Object {
  curl.exe -i -s "http://localhost:8080/api/users/search?mask=iv" `
    -H "Authorization: Bearer $env:TOKEN" |
    Select-String -Pattern "HTTP/|X-RateLimit|rate limit"
}
```

## MongoDB scripts

Создать коллекции, validators и индексы:

```bash
docker compose exec mongo mongosh /scripts/validation.js
```

Загрузить тестовые документы:

```bash
docker compose exec mongo mongosh /scripts/data.js
```

Выполнить CRUD-запросы и aggregation pipeline:

```bash
docker compose exec mongo mongosh /scripts/queries.js
```

Проверить MongoDB endpoints из ЛР4:

```bash
curl -i http://localhost:8080/api/goals/1/tasks/1/comments \
  -H "Authorization: Bearer $TOKEN"

curl -i http://localhost:8080/api/goals/1/tasks/1/activity \
  -H "Authorization: Bearer $TOKEN"
```

## PostgreSQL checks

Открыть `psql` внутри контейнера:

```bash
docker compose exec postgres psql -U task_planning -d task_planning
```

Проверить таблицы:

```bash
docker compose exec -T postgres psql -U task_planning -d task_planning \
  -c "\dt"
```

Проверить количество записей:

```bash
docker compose exec -T postgres psql -U task_planning -d task_planning \
  -c "SELECT 'users' AS table_name, COUNT(*) FROM users UNION ALL SELECT 'goals', COUNT(*) FROM goals UNION ALL SELECT 'tasks', COUNT(*) FROM tasks;"
```
