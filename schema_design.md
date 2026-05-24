# Лабораторная работа 04. Документная модель MongoDB

## Назначение MongoDB

Вариант 10 описывает сервис планирования задач с основными сущностями
`users`, `goals` и `tasks`. Эти сущности остаются в PostgreSQL, потому что для
них важны связи, ограничения целостности, транзакционное обновление статусов и
поиск по нормализованным полям.

MongoDB используется для данных, которые естественно выглядят как документы и
часто читаются вместе с задачей:

- `task_activity` - история событий задачи;
- `task_comments` - комментарии к задаче;
- `notification_log` - журнал отправки уведомлений.

## Коллекция task_activity

Документ хранит одно событие в истории задачи:

```javascript
{
  taskId: Long('1'),
  goalId: Long('1'),
  type: 'status_changed',
  actor: {
    userId: Long('2'),
    login: 'maria',
    displayName: 'Maria Orlova'
  },
  payload: {
    oldStatus: 'new',
    newStatus: 'in_progress'
  },
  visibleTo: [Long('1'), Long('2')],
  important: true,
  createdAt: ISODate('2026-05-20T10:15:00Z')
}
```

`taskId` и `goalId` являются ссылками на PostgreSQL. Поля `actor` и `payload`
встроены, потому что они описывают конкретный факт истории и должны сохранять
снимок данных на момент события.

## Коллекция task_comments

Документ хранит комментарий к задаче:

```javascript
{
  taskId: Long('1'),
  goalId: Long('1'),
  author: {
    userId: Long('1'),
    login: 'alexey',
    displayName: 'Alexey Sokolov'
  },
  text: 'Need to describe all allowed status transitions.',
  tags: ['requirements', 'status'],
  createdAt: ISODate('2026-05-20T08:30:00Z'),
  updatedAt: ISODate('2026-05-20T08:30:00Z')
}
```

`author` встроен как снимок автора комментария. `tags` встроены массивом строк,
потому что они принадлежат только одному комментарию и удобно обновляются через
`$addToSet` и `$pull`.

Комментарии не встраиваются в `tasks`, потому что задачи хранятся в PostgreSQL,
а комментариев у задачи может быть много. Отдельная коллекция позволяет
пагинировать, сортировать и удалять комментарии без изменения основной записи
задачи.

## Коллекция notification_log

Документ хранит результат или состояние отправки уведомления:

```javascript
{
  recipient: {
    userId: Long('2'),
    login: 'maria',
    displayName: 'Maria Orlova'
  },
  channel: 'email',
  message: {
    subject: 'Task status changed',
    body: 'Task #1 moved to in_progress.'
  },
  taskId: Long('1'),
  goalId: Long('1'),
  status: 'sent',
  attempts: 1,
  delivered: true,
  createdAt: ISODate('2026-05-20T10:16:00Z'),
  sentAt: ISODate('2026-05-20T10:16:10Z')
}
```

`recipient` и `message` встроены, потому что журнал должен хранить фактическое
уведомление в том виде, в котором оно было подготовлено. `recipient.userId`
остается ссылкой на пользователя PostgreSQL/API.

## Embedded vs References

В PostgreSQL остаются основные идентификаторы и связи:

- `taskId` - ссылка на `tasks.id`;
- `goalId` - ссылка на `goals.id`;
- `userId` - ссылка на `users.id`.

В MongoDB встраиваются данные, которые нужны вместе с документом:

- `author`, `actor`, `recipient` - снимки пользователя для комментариев,
  истории и уведомлений;
- `payload` - произвольные детали события активности;
- `message` - текст и тема уведомления;
- `tags` - массив меток комментария.

Такое разделение оставляет PostgreSQL источником истины для основных сущностей,
а MongoDB - хранилищем документной истории, комментариев и журнала событий.
