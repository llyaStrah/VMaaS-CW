FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends g++ libpq-dev librdkafka-dev nlohmann-json3-dev && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY backend/mvp/main.cpp ./main.cpp
COPY backend/mvp/worker.cpp ./worker.cpp
RUN g++ -I/usr/include/postgresql -std=c++20 -O2 -Wall -Wextra -Wpedantic main.cpp -o /vmaas-api -lpq -lrdkafka && g++ -I/usr/include/postgresql -std=c++20 -O2 -Wall -Wextra -Wpedantic worker.cpp -o /vmaas-worker -lpq -lrdkafka
FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends libpq5 librdkafka1 ca-certificates && rm -rf /var/lib/apt/lists/*
COPY --from=build /vmaas-api /usr/local/bin/vmaas-api
COPY --from=build /vmaas-worker /usr/local/bin/vmaas-worker
EXPOSE 8080
ENTRYPOINT ["/usr/local/bin/vmaas-api"]

