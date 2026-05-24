const database = db.getSiblingDB('task_planning_mongo');
const now = new Date();

print('Create: add task comment');
const commentResult = database.task_comments.insertOne({
  taskId: Long('1'),
  goalId: Long('1'),
  author: { userId: Long('1'), login: 'alexey', displayName: 'Alexey Sokolov' },
  text: 'MongoDB query script comment.',
  tags: ['lab4', 'mongodb'],
  createdAt: now,
  updatedAt: now,
});
printjson(commentResult);

print('Create: add task activity event');
const activityResult = database.task_activity.insertOne({
  taskId: Long('1'),
  goalId: Long('1'),
  type: 'status_changed',
  actor: { userId: Long('1'), login: 'alexey', displayName: 'Alexey Sokolov' },
  payload: { oldStatus: 'done', newStatus: 'in_progress', source: 'queries.js' },
  visibleTo: [Long('1'), Long('2')],
  important: true,
  createdAt: now,
});
printjson(activityResult);

print('Create: add notification log record');
const notificationResult = database.notification_log.insertOne({
  recipient: { userId: Long('2'), login: 'maria', displayName: 'Maria Orlova' },
  channel: 'web',
  message: {
    subject: 'Status changed',
    body: 'Task #1 status was changed by queries.js.',
  },
  taskId: Long('1'),
  goalId: Long('1'),
  status: 'queued',
  attempts: 0,
  delivered: false,
  createdAt: now,
});
printjson(notificationResult);

print('Read: comments for taskId using $eq, $ne, $and');
printjson(
  database.task_comments
    .find({
      $and: [
        { taskId: { $eq: Long('1') } },
        { tags: { $ne: 'archived' } },
      ],
    })
    .sort({ createdAt: 1 })
    .toArray(),
);

print('Read: activity history using $eq, $gt, $lt');
printjson(
  database.task_activity
    .find({
      taskId: { $eq: Long('1') },
      createdAt: {
        $gt: new Date('2026-01-01T00:00:00Z'),
        $lt: new Date('2027-01-01T00:00:00Z'),
      },
    })
    .sort({ createdAt: 1 })
    .toArray(),
);

print('Read: notifications by recipient.userId using $or');
printjson(
  database.notification_log
    .find({
      $or: [
        { 'recipient.userId': { $eq: Long('2') } },
        { status: { $eq: 'failed' } },
      ],
    })
    .sort({ createdAt: -1 })
    .toArray(),
);

print('Read: status_changed events using $in');
printjson(
  database.task_activity
    .find({ type: { $in: ['status_changed'] } })
    .sort({ createdAt: -1 })
    .toArray(),
);

print('Update: update comment text');
printjson(
  database.task_comments.updateOne(
    { _id: commentResult.insertedId },
    {
      $set: {
        text: 'Updated MongoDB query script comment.',
        updatedAt: new Date(),
      },
    },
  ),
);

print('Update: add tag with $addToSet');
printjson(
  database.task_comments.updateOne(
    { _id: commentResult.insertedId },
    { $addToSet: { tags: 'reviewed' } },
  ),
);

print('Update: remove tag with $pull');
printjson(
  database.task_comments.updateOne(
    { _id: commentResult.insertedId },
    { $pull: { tags: 'mongodb' } },
  ),
);

print('Update: change notification status');
printjson(
  database.notification_log.updateOne(
    { _id: notificationResult.insertedId },
    {
      $set: {
        status: 'sent',
        delivered: true,
        attempts: 1,
        sentAt: new Date(),
      },
    },
  ),
);

print('Delete: delete comment');
printjson(database.task_comments.deleteOne({ _id: commentResult.insertedId }));

print('Delete: delete old notification_log records by date');
database.notification_log.insertOne({
  recipient: { userId: Long('1'), login: 'alexey', displayName: 'Alexey Sokolov' },
  channel: 'email',
  message: { subject: 'Old notification', body: 'This record is used for delete query.' },
  taskId: Long('1'),
  goalId: Long('1'),
  status: 'sent',
  attempts: 1,
  delivered: true,
  createdAt: new Date('2024-01-01T00:00:00Z'),
});
printjson(
  database.notification_log.deleteMany({
    createdAt: { $lt: new Date('2025-01-01T00:00:00Z') },
  }),
);

print('Aggregation: group task_activity by taskId and type');
printjson(
  database.task_activity
    .aggregate([
      {
        $group: {
          _id: { taskId: '$taskId', type: '$type' },
          eventsCount: { $sum: 1 },
        },
      },
      { $sort: { eventsCount: -1, '_id.taskId': 1, '_id.type': 1 } },
    ])
    .toArray(),
);
