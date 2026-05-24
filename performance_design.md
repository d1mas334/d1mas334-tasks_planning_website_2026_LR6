# Performance Design

## 1. Анализ производительности

Вариант 10 - планирование задач. Основные сущности приложения: цель, задача, исполнитель. ЛР5 добавляет Redis 7 для кеширования read-heavy endpoints и для rate limiting.

### Hot paths

| Endpoint | Почему горячий путь |
|---|---|
| `GET /api/goals` | Часто открывается на главном экране планирования, возвращает список целей. |
| `GET /api/goals/{goalId}/tasks` | Часто вызывается при просмотре цели, формирует рабочий список задач. |
| `GET /api/users/search?mask=...` | Используется при поиске исполнителей, может получать серии запросов при вводе маски. |
| `GET /api/goals/{goalId}/tasks/{taskId}/comments` | Читает документные данные из MongoDB при просмотре карточки задачи. |
| `PATCH /api/goals/{goalId}/tasks/{taskId}/status` | Частая операция изменения состояния задачи, влияет на список задач и activity log. |

### Slow operations

| Операция | Причина задержки |
|---|---|
| PostgreSQL read | Сетевой поход в БД, выполнение SQL, сериализация результата в JSON. |
| `LIKE '%mask%'` в `GET /api/users/search` | Маска с ведущим `%` хуже использует обычные B-tree индексы и может сканировать больше строк. |
| MongoDB comments/activity reads | Сетевой поход в MongoDB и сортировка документных данных по времени создания. |
| Cache miss | При промахе Redis приложение дополнительно читает PostgreSQL и заполняет кеш. |

### Целевые метрики

| Метрика | Целевое значение |
|---|---|
| Cache hit response | `< 50 ms` |
| DB read response | `< 200 ms` |
| 95 percentile latency | `< 300 ms` |
| Throughput для cached reads | Выше DB-only варианта за счет отсутствия повторных чтений PostgreSQL. |
| Throughput для `GET /api/users/search` | Не более 60 запросов в минуту на авторизованного пользователя. |

## 2. Стратегия кеширования

Используется Cache-Aside / Lazy Loading: обработчик сначала проверяет Redis. Если ключ найден, API возвращает JSON из Redis. Если ключ отсутствует, API читает PostgreSQL, формирует JSON, сохраняет его в Redis с TTL и возвращает клиенту.

| Endpoint | Redis key | TTL | Strategy | Invalidation |
|---|---|---:|---|---|
| `GET /api/goals` | `goals:all` | 60 секунд | Cache-Aside | `POST /api/goals` удаляет `goals:all`. |
| `GET /api/goals/{goalId}/tasks` | `goal:{goalId}:tasks` | 30 секунд | Cache-Aside | `POST /api/goals/{goalId}/tasks` и `PATCH /api/goals/{goalId}/tasks/{taskId}/status` удаляют `goal:{goalId}:tasks`. |

Кеш хранит готовый JSON ответа. Это снижает не только нагрузку на PostgreSQL, но и повторную стоимость сборки JSON в приложении.

## 3. Обоснование Cache-Aside

Cache-Aside выбран потому что он просто реализуется в REST API, не требует отдельного фонового процесса и оставляет контроль инвалидации в приложении. Стратегия подходит для read-heavy endpoints: списки целей и задач читаются чаще, чем изменяются. TTL ограничивает срок жизни устаревших данных, а явная инвалидация после успешных write operations удаляет наиболее вероятные stale keys сразу.

## 4. Rate limiting

Для `GET /api/users/search?mask=...` используется Fixed Window Counter через Redis.

| Параметр | Значение |
|---|---|
| Endpoint | `GET /api/users/search?mask=...` |
| Scope | Авторизованный пользователь из bearer token `token-{userId}` |
| Key | `rate:user-search:{userId}:{windowStartUnixMinute}` |
| Limit | 60 запросов в минуту |
| Window | 60 секунд |
| Redis operations | `INCR`; при первом запросе в окне `EXPIRE 60` |
| Headers | `X-RateLimit-Limit`, `X-RateLimit-Remaining`, `X-RateLimit-Reset` |
| 429 behavior | HTTP `429 Too Many Requests`, JSON `{"error":"rate limit exceeded"}` |

Fixed Window Counter выбран из-за простоты, атомарного `INCR` в Redis и достаточной точности для учебного API. Для production-сценария с более строгой равномерностью нагрузки можно заменить алгоритм на sliding window или token bucket.

## 5. Метрики мониторинга

Для оценки эффективности нужно собирать:

- cache hit rate;
- cache miss rate;
- Redis latency;
- PostgreSQL query latency;
- number of 429 responses;
- p95/p99 latency по endpoints;
- latency отдельно для `X-Cache: HIT` и `X-Cache: MISS`.

## 6. Как измерять эффективность

1. Выполнить `GET /api/goals` два раза подряд: первый ответ должен вернуть `X-Cache: MISS`, второй - `X-Cache: HIT`.
2. Выполнить `GET /api/goals/1/tasks` два раза подряд: первый ответ должен вернуть `X-Cache: MISS`, второй - `X-Cache: HIT`.
3. Сравнить время ответов `MISS` и `HIT`; cached response должен быть заметно быстрее, так как не читает PostgreSQL.
4. Посчитать `hit rate = hits / (hits + misses)` по заголовкам `X-Cache`.
5. Проверить инвалидацию: после `POST /api/goals` следующий `GET /api/goals` снова должен быть `MISS`; после `POST /api/goals/1/tasks` следующий `GET /api/goals/1/tasks` снова должен быть `MISS`.
6. Для rate limiting выполнить больше 60 запросов к `GET /api/users/search?mask=iv` с `Authorization` header. После превышения лимита API должен вернуть `429` и rate-limit headers.
7. Для нагрузочного тестирования можно использовать shell loop, `ab` или `wrk`, если они доступны в окружении.
