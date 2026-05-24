const dbName = 'task_planning_mongo';
const database = db.getSiblingDB(dbName);

function ensureCollection(name, validator) {
  const exists = database.getCollectionNames().includes(name);

  if (!exists) {
    database.createCollection(name, { validator });
    print(`created collection ${name}`);
    return;
  }

  database.runCommand({
    collMod: name,
    validator,
    validationLevel: 'strict',
    validationAction: 'error',
  });
  print(`updated validator for ${name}`);
}

ensureCollection('task_comments', {
  $jsonSchema: {
    bsonType: 'object',
    required: ['taskId', 'goalId', 'author', 'text', 'createdAt'],
    additionalProperties: true,
    properties: {
      taskId: {
        bsonType: ['int', 'long'],
        description: 'PostgreSQL tasks.id reference is required',
      },
      goalId: {
        bsonType: ['int', 'long'],
        description: 'PostgreSQL goals.id reference is required',
      },
      author: {
        bsonType: 'object',
        required: ['userId', 'login', 'displayName'],
        properties: {
          userId: {
            bsonType: ['int', 'long'],
            description: 'PostgreSQL users.id reference',
          },
          login: { bsonType: 'string' },
          displayName: { bsonType: 'string' },
        },
      },
      text: {
        bsonType: 'string',
        minLength: 1,
        maxLength: 2000,
      },
      tags: {
        bsonType: 'array',
        items: { bsonType: 'string' },
      },
      createdAt: { bsonType: 'date' },
      updatedAt: { bsonType: 'date' },
    },
  },
});

ensureCollection('task_activity', {
  $jsonSchema: {
    bsonType: 'object',
    required: ['taskId', 'goalId', 'type', 'actor', 'payload', 'createdAt'],
    additionalProperties: true,
    properties: {
      taskId: { bsonType: ['int', 'long'] },
      goalId: { bsonType: ['int', 'long'] },
      type: {
        bsonType: 'string',
        enum: [
          'task_created',
          'status_changed',
          'assignee_changed',
          'comment_added',
          'deadline_changed',
        ],
      },
      actor: {
        bsonType: 'object',
        required: ['userId'],
        properties: {
          userId: { bsonType: ['int', 'long'] },
          login: { bsonType: 'string' },
          displayName: { bsonType: 'string' },
        },
      },
      payload: { bsonType: 'object' },
      visibleTo: {
        bsonType: 'array',
        items: { bsonType: ['int', 'long'] },
      },
      important: { bsonType: 'bool' },
      createdAt: { bsonType: 'date' },
    },
  },
});

ensureCollection('notification_log', {
  $jsonSchema: {
    bsonType: 'object',
    required: ['recipient', 'channel', 'message', 'status', 'createdAt'],
    additionalProperties: true,
    properties: {
      recipient: {
        bsonType: 'object',
        required: ['userId', 'login'],
        properties: {
          userId: { bsonType: ['int', 'long'] },
          login: { bsonType: 'string' },
          displayName: { bsonType: 'string' },
        },
      },
      channel: {
        bsonType: 'string',
        enum: ['email', 'web', 'telegram', 'sms'],
      },
      message: {
        bsonType: 'object',
        required: ['subject', 'body'],
        properties: {
          subject: { bsonType: 'string' },
          body: { bsonType: 'string' },
        },
      },
      taskId: { bsonType: ['int', 'long'] },
      goalId: { bsonType: ['int', 'long'] },
      status: {
        bsonType: 'string',
        enum: ['queued', 'sent', 'failed', 'read'],
      },
      attempts: { bsonType: ['int', 'long'] },
      delivered: { bsonType: 'bool' },
      createdAt: { bsonType: 'date' },
      sentAt: { bsonType: 'date' },
      readAt: { bsonType: 'date' },
    },
  },
});

database.task_comments.createIndex({ taskId: 1, createdAt: 1 });
database.task_activity.createIndex({ taskId: 1, type: 1, createdAt: -1 });
database.notification_log.createIndex({ 'recipient.userId': 1, createdAt: -1 });
database.notification_log.createIndex({ status: 1, createdAt: -1 });

print(`validation is ready for ${dbName}`);
