import json
import os
import signal
import sys
import time
from datetime import datetime, timezone

import pika
from pymongo import ASCENDING, MongoClient
from pymongo.errors import DuplicateKeyError


EXCHANGE = os.getenv("RABBITMQ_EXCHANGE", "task_planning.events")
QUEUE = os.getenv("RABBITMQ_QUEUE", "audit.queue")


def log(message: str) -> None:
    print(f"[event-worker] {message}", flush=True)


def parse_event_datetime(value: str) -> datetime:
    if not value:
        return datetime.now(timezone.utc)
    return datetime.fromisoformat(value.replace("Z", "+00:00"))


def connect_rabbitmq() -> pika.BlockingConnection:
    credentials = pika.PlainCredentials(
        os.getenv("RABBITMQ_USER", "task_planning"),
        os.getenv("RABBITMQ_PASSWORD", "task_planning"),
    )
    parameters = pika.ConnectionParameters(
        host=os.getenv("RABBITMQ_HOST", "rabbitmq"),
        port=int(os.getenv("RABBITMQ_PORT", "5672")),
        virtual_host=os.getenv("RABBITMQ_VHOST", "/"),
        credentials=credentials,
        heartbeat=30,
        blocked_connection_timeout=30,
    )
    return pika.BlockingConnection(parameters)


def declare_topology(channel: pika.channel.Channel) -> None:
    channel.exchange_declare(exchange=EXCHANGE, exchange_type="topic", durable=True)

    bindings = {
        "notification.queue": [
            "task.created",
            "task.status_changed",
            "task.comment_added",
            "notification.requested",
        ],
        "read_model.queue": [
            "goal.created",
            "task.created",
            "task.status_changed",
            "task.comment_added",
        ],
        "audit.queue": ["#"],
    }

    for queue_name, routing_keys in bindings.items():
        channel.queue_declare(queue=queue_name, durable=True)
        for routing_key in routing_keys:
            channel.queue_bind(
                queue=queue_name,
                exchange=EXCHANGE,
                routing_key=routing_key,
            )


class EventProjector:
    def __init__(self) -> None:
        mongo_uri = os.getenv(
            "MONGO_URI", "mongodb://mongo:27017/task_planning_mongo"
        )
        self.client = MongoClient(mongo_uri)
        self.database = self.client.get_database("task_planning_mongo")
        self.event_log = self.database.event_log
        self.task_activity = self.database.task_activity
        self.event_log.create_index([("eventId", ASCENDING)], unique=True)
        self.event_log.create_index([("eventType", ASCENDING), ("occurredAt", ASCENDING)])

    def process(self, event: dict) -> bool:
        event_id = event.get("eventId")
        event_type = event.get("eventType")
        occurred_at = parse_event_datetime(event.get("occurredAt", ""))

        if not event_id or not event_type:
            raise ValueError("eventId and eventType are required")

        try:
            self.event_log.insert_one(
                {
                    "eventId": event_id,
                    "eventType": event_type,
                    "occurredAt": occurred_at,
                    "producer": event.get("producer"),
                    "version": event.get("version", 1),
                    "payload": event.get("payload", {}),
                    "processedAt": datetime.now(timezone.utc),
                    "status": "processed",
                }
            )
        except DuplicateKeyError:
            log(f"duplicate event ignored eventId={event_id} type={event_type}")
            return False

        payload = event.get("payload", {})
        if event_type == "task.created":
            self._project_task_created(payload, occurred_at, event_id)
        elif event_type == "task.status_changed":
            self._project_status_changed(payload, occurred_at, event_id)
        elif event_type == "task.comment_added":
            self._project_comment_added(payload, occurred_at, event_id)

        return True

    def _project_task_created(
        self, payload: dict, occurred_at: datetime, event_id: str
    ) -> None:
        self.task_activity.insert_one(
            {
                "taskId": payload.get("taskId"),
                "goalId": payload.get("goalId"),
                "type": "task_created",
                "actor": {"userId": payload.get("authorUserId")},
                "payload": {
                    "eventId": event_id,
                    "title": payload.get("title"),
                    "initialStatus": payload.get("status"),
                    "assigneeUserId": payload.get("assigneeUserId"),
                    "dueDate": payload.get("dueDate"),
                },
                "visibleTo": [
                    value
                    for value in [
                        payload.get("authorUserId"),
                        payload.get("assigneeUserId"),
                    ]
                    if value is not None
                ],
                "important": False,
                "createdAt": occurred_at,
            }
        )

    def _project_status_changed(
        self, payload: dict, occurred_at: datetime, event_id: str
    ) -> None:
        self.task_activity.insert_one(
            {
                "taskId": payload.get("taskId"),
                "goalId": payload.get("goalId"),
                "type": "status_changed",
                "actor": {"userId": payload.get("actorUserId")},
                "payload": {
                    "eventId": event_id,
                    "oldStatus": payload.get("oldStatus"),
                    "newStatus": payload.get("newStatus"),
                },
                "visibleTo": [
                    value
                    for value in [payload.get("actorUserId")]
                    if value is not None
                ],
                "important": True,
                "createdAt": occurred_at,
            }
        )

    def _project_comment_added(
        self, payload: dict, occurred_at: datetime, event_id: str
    ) -> None:
        self.task_activity.insert_one(
            {
                "taskId": payload.get("taskId"),
                "goalId": payload.get("goalId"),
                "type": "comment_added",
                "actor": {"userId": payload.get("actorUserId")},
                "payload": {
                    "eventId": event_id,
                    "commentId": payload.get("commentId"),
                    "textPreview": payload.get("textPreview"),
                    "tags": payload.get("tags", []),
                },
                "visibleTo": [
                    value
                    for value in [
                        payload.get("actorUserId"),
                        payload.get("assigneeUserId"),
                    ]
                    if value is not None
                ],
                "important": False,
                "createdAt": occurred_at,
            }
        )


def main() -> int:
    projector = EventProjector()

    while True:
        try:
            connection = connect_rabbitmq()
            channel = connection.channel()
            declare_topology(channel)
            channel.basic_qos(prefetch_count=10)
            log(f"listening queue={QUEUE} exchange={EXCHANGE}")

            def handle_message(ch, method, properties, body):
                try:
                    event = json.loads(body.decode("utf-8"))
                    processed = projector.process(event)
                    event_type = event.get("eventType", "unknown")
                    event_id = event.get("eventId", "unknown")
                    log(
                        f"acked eventId={event_id} type={event_type} "
                        f"processed={processed}"
                    )
                    ch.basic_ack(delivery_tag=method.delivery_tag)
                except Exception as error:
                    log(f"failed to process message: {error}")
                    ch.basic_nack(delivery_tag=method.delivery_tag, requeue=True)

            channel.basic_consume(queue=QUEUE, on_message_callback=handle_message)

            def stop(signum, frame):
                log("shutdown requested")
                channel.stop_consuming()

            signal.signal(signal.SIGTERM, stop)
            signal.signal(signal.SIGINT, stop)
            channel.start_consuming()
            connection.close()
            return 0
        except Exception as error:
            log(f"connection failed: {error}; retrying in 5s")
            time.sleep(5)


if __name__ == "__main__":
    sys.exit(main())
