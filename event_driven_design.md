# Лабораторная работа 06. Проектирование Event-Driven архитектуры

## Контекст

Дисциплина: Архитектура информационных систем / Программная инженерия.

Вариант 10: планирование задач. Основные сущности приложения:

- пользователь / исполнитель;
- цель;
- задача.

ЛР6 развивает REST API из ЛР5. Write endpoints остаются command handlers: они проверяют входные данные, выполняют запись в PostgreSQL или MongoDB, инвалидируют Redis cache при необходимости и после успешной операции публикуют domain event в RabbitMQ.

## Commands и события

| Command / endpoint | Хранилище command model | Событие |
|---|---|---|
| `POST /api/users` | PostgreSQL `users` | `user.created` |
| `POST /api/goals` | PostgreSQL `goals` | `goal.created` |
| `POST /api/goals/{goalId}/tasks` | PostgreSQL `tasks` | `task.created` |
| `PATCH /api/goals/{goalId}/tasks/{taskId}/status` | PostgreSQL `tasks`, MongoDB `task_activity` | `task.status_changed` |
| `POST /api/goals/{goalId}/tasks/{taskId}/comments` | MongoDB `task_comments` | `task.comment_added` |

События `notification.requested` и `read_model.updated` описаны в каталоге как события расширения архитектуры: первое может создаваться notification-сервисом, второе - read model projector после обновления MongoDB-проекции.

## Producer

Producer находится в основном C++20 / Yandex Userver API. Компонент `event-publisher` использует системную библиотеку `rabbitmq-c`:

- объявляет exchange и очереди при старте;
- после успешной write-операции формирует JSON event;
- публикует сообщение в `task_planning.events`;
- использует routing key, равный имени события;
- отправляет persistent messages (`delivery_mode = 2`).

Ошибки публикации логируются. Учебная реализация не откатывает уже выполненную запись в PostgreSQL/MongoDB, чтобы не ломать существующее поведение ЛР5. В промышленном варианте для строгой надежности добавляется transactional outbox.

## Consumer

Consumer реализован отдельным процессом `event-worker`:

- запускается как отдельный compose service;
- подключается к RabbitMQ через AMQP;
- читает `audit.queue`;
- использует manual ack;
- пишет каждое новое событие в MongoDB `event_log`;
- обрабатывает дубликаты по уникальному `eventId`;
- для `task.created`, `task.status_changed`, `task.comment_added` обновляет MongoDB `task_activity`.

Worker написан на Python с `pika` и `pymongo` только для AMQP integration и фоновой проекции событий. Основной API остается C++20 / Yandex Userver.

## RabbitMQ topology

Exchange:

| Name | Type | Durable |
|---|---|---|
| `task_planning.events` | `topic` | yes |

Queues:

| Queue | Durable | Назначение |
|---|---:|---|
| `notification.queue` | yes | события, из которых можно формировать уведомления |
| `read_model.queue` | yes | события для обновления CQRS read model |
| `audit.queue` | yes | полный аудит всех событий |

Bindings:

| Queue | Routing keys |
|---|---|
| `notification.queue` | `task.created`, `task.status_changed`, `task.comment_added`, `notification.requested` |
| `read_model.queue` | `goal.created`, `task.created`, `task.status_changed`, `task.comment_added` |
| `audit.queue` | `#` |

Текущий worker читает `audit.queue`, потому что для лабораторной работы достаточно одного consumer-процесса, который видит все события и может строить аудит.

## Формат сообщения

Все сообщения публикуются в JSON:

```json
{
  "eventId": "evt-1780000000000-1",
  "eventType": "task.status_changed",
  "occurredAt": "2026-05-24T10:00:00Z",
  "producer": "task-planning-api",
  "version": 1,
  "payload": {
    "taskId": 1,
    "goalId": 1,
    "actorUserId": 1,
    "oldStatus": "new",
    "newStatus": "in_progress"
  }
}
```

`eventId` генерируется API как timestamp-based id с локальным счетчиком. Этого достаточно для лабораторной работы и проверки idempotency.

## Поток событий

Текстовая схема взаимодействия:

```text
Client
  -> REST API command endpoint
  -> PostgreSQL / MongoDB write
  -> Redis cache invalidation if needed
  -> RabbitMQ topic exchange task_planning.events
  -> durable queues by routing key
  -> event-worker consumes audit.queue
  -> MongoDB event_log for idempotency and audit
  -> MongoDB task_activity as read model projection
  -> REST API read endpoints return PostgreSQL/MongoDB data, Redis remains cache layer
```

Пример для смены статуса:

```text
PATCH /api/goals/1/tasks/1/status
  -> UPDATE tasks SET status = ...
  -> DEL goal:1:tasks
  -> publish task.status_changed
  -> event-worker ack после записи event_log/task_activity
  -> GET /api/goals/1/tasks/1/activity показывает новую активность
```

## Delivery guarantees

Гарантии доставки в текущей реализации:

- at-least-once delivery;
- durable exchange `task_planning.events`;
- durable queues;
- persistent messages;
- manual ack в consumer;
- `basic_nack(requeue=True)` при ошибке обработки;
- idempotency через `eventId`;
- duplicate handling через уникальный индекс MongoDB `event_log.eventId`.

Ограничение учебной версии: producer публикует событие после записи в БД без transactional outbox. Если API упадет между записью и publish, событие может быть потеряно. В production-варианте command handler сначала пишет запись и outbox-событие в одной транзакции PostgreSQL, а отдельный relay публикует outbox в RabbitMQ.

## CQRS

Command/write model:

- PostgreSQL `users`;
- PostgreSQL `goals`;
- PostgreSQL `tasks`;
- MongoDB `task_comments` для команд добавления комментариев.

Query/read model:

- MongoDB `task_activity`;
- MongoDB `task_comments`;
- MongoDB `notification_log`;
- MongoDB `event_log` как audit/idempotency log.

Связь:

- command handlers принимают REST-команды;
- write model фиксирует состояние;
- RabbitMQ распространяет domain events;
- worker обновляет MongoDB read model;
- read endpoints могут читать PostgreSQL/MongoDB;
- Redis остается cache layer для горячих read endpoints `GET /api/goals` и `GET /api/goals/{goalId}/tasks`.

## Как тестировать

1. Проверить compose-файл:

```bash
docker compose config
```

2. Собрать и запустить:

```bash
docker compose up --build -d
docker compose ps
```

3. Проверить API:

```bash
curl http://localhost:8080/ping
```

4. Получить token:

```bash
TOKEN=$(curl -s -X POST http://localhost:8080/api/auth/login \
  -H "Content-Type: application/json" \
  -d '{"login":"alexey","password":"pass123"}' | sed -E 's/.*"token":"([^"]+)".*/\1/')
```

5. Создать цель, задачу, сменить статус и добавить комментарий:

```bash
curl -i -X POST http://localhost:8080/api/goals \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer $TOKEN" \
  -d '{"title":"Event driven goal","description":"Check goal.created event"}'

curl -i -X POST http://localhost:8080/api/goals/1/tasks \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer $TOKEN" \
  -d '{"title":"Event driven task","description":"Check task.created event","assigneeId":2,"dueDate":"2026-06-30"}'

curl -i -X PATCH http://localhost:8080/api/goals/1/tasks/1/status \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer $TOKEN" \
  -d '{"status":"in_progress"}'

curl -i -X POST http://localhost:8080/api/goals/1/tasks/1/comments \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer $TOKEN" \
  -d '{"text":"Comment that should produce task.comment_added","tags":["event","rabbitmq"]}'
```

6. Посмотреть worker logs:

```bash
docker compose logs event-worker
```

7. Проверить MongoDB `event_log` и `task_activity`:

```bash
docker compose exec -T mongo mongosh --quiet --eval \
  "db.getSiblingDB('task_planning_mongo').event_log.find({}, {eventId:1,eventType:1,processedAt:1}).sort({processedAt:-1}).limit(10).toArray()"

docker compose exec -T mongo mongosh --quiet --eval \
  "db.getSiblingDB('task_planning_mongo').task_activity.find({taskId: 1}).sort({createdAt:-1}).limit(10).toArray()"
```

8. Открыть RabbitMQ Management UI:

```text
http://localhost:15672
login: task_planning
password: task_planning
```
