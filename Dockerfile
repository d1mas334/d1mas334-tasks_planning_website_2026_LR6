FROM ghcr.io/userver-framework/ubuntu-22.04-userver:latest AS build

WORKDIR /app
RUN apt-get update && \
    apt-get install -y --no-install-recommends librabbitmq-dev && \
    rm -rf /var/lib/apt/lists/*
COPY . .

RUN cmake -S . -B build -GNinja -DCMAKE_BUILD_TYPE=Release
RUN cmake --build build --target task_planning_api

FROM ghcr.io/userver-framework/ubuntu-22.04-userver:latest

WORKDIR /app
RUN apt-get update && \
    apt-get install -y --no-install-recommends librabbitmq4 python3 python3-pip && \
    python3 -m pip install --no-cache-dir pika pymongo && \
    rm -rf /var/lib/apt/lists/*
COPY --from=build /app/build/task_planning_api /app/task_planning_api
COPY configs /app/configs
COPY db /app/db
COPY worker /app/worker

EXPOSE 8080

CMD ["/app/task_planning_api", "-c", "/app/configs/static_config.yaml"]
