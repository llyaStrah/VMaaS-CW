#include "compute_provider.hpp"

#include <libpq-fe.h>
#include <librdkafka/rdkafka.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

using json = nlohmann::json;

namespace {
std::string env(const char* key, const char* fallback) {
    const char* value = std::getenv(key);
    return value ? value : fallback;
}

PGconn* connect_db(const std::string& url) {
    auto* db = PQconnectdb(url.c_str());
    if (PQstatus(db) != CONNECTION_OK) {
        std::cerr << "worker database connection failed: " << PQerrorMessage(db) << std::endl;
        PQfinish(db);
        return nullptr;
    }
    return db;
}

bool exec_ok(PGconn* db, const char* sql, int count = 0, const char* const* values = nullptr) {
    PGresult* result = PQexecParams(db, sql, count, nullptr, values, nullptr, nullptr, 0);
    const bool ok = result && (PQresultStatus(result) == PGRES_COMMAND_OK || PQresultStatus(result) == PGRES_TUPLES_OK);
    if (!ok) std::cerr << "worker SQL failed: " << PQerrorMessage(db) << std::endl;
    if (result) PQclear(result);
    return ok;
}

void outbox_loop(const std::string& db_url, const std::string& brokers) {
    char error[512];
    rd_kafka_conf_t* config = rd_kafka_conf_new();
    if (rd_kafka_conf_set(config, "bootstrap.servers", brokers.c_str(), error, sizeof(error)) != RD_KAFKA_CONF_OK) {
        std::cerr << "outbox producer config failed: " << error << std::endl;
        return;
    }
    rd_kafka_t* producer = rd_kafka_new(RD_KAFKA_PRODUCER, config, error, sizeof(error));
    if (!producer) { std::cerr << "outbox producer init failed: " << error << std::endl; return; }
    PGconn* db = nullptr;
    while (true) {
        if (!db || PQstatus(db) != CONNECTION_OK) {
            if (db) PQfinish(db);
            db = connect_db(db_url);
            if (!db) { std::this_thread::sleep_for(std::chrono::seconds(3)); continue; }
        }
        PGresult* rows = PQexec(db, "SELECT id::text,aggregate_id,payload::text FROM outbox_events WHERE published_at IS NULL ORDER BY created_at LIMIT 20");
        if (!rows || PQresultStatus(rows) != PGRES_TUPLES_OK) {
            if (rows) PQclear(rows);
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }
        for (int i = 0; i < PQntuples(rows); ++i) {
            const std::string event_id = PQgetvalue(rows, i, 0);
            const std::string order_id = PQgetvalue(rows, i, 1);
            const std::string payload = PQgetvalue(rows, i, 2);
            const auto produced = rd_kafka_producev(producer,
                RD_KAFKA_V_TOPIC("vm.provision.requested"),
                RD_KAFKA_V_MSGFLAGS(RD_KAFKA_MSG_F_COPY),
                RD_KAFKA_V_VALUE(const_cast<char*>(payload.data()), payload.size()),
                RD_KAFKA_V_KEY(const_cast<char*>(order_id.data()), order_id.size()), RD_KAFKA_V_END);
            if (produced != RD_KAFKA_RESP_ERR_NO_ERROR) {
                std::cerr << "outbox publish enqueue failed: " << rd_kafka_err2str(produced) << std::endl;
                continue;
            }
            if (rd_kafka_flush(producer, 10000) == RD_KAFKA_RESP_ERR_NO_ERROR) {
                const char* values[] = {event_id.c_str()};
                exec_ok(db, "UPDATE outbox_events SET published_at=now(),attempts=attempts+1 WHERE id=$1::uuid AND published_at IS NULL", 1, values);
                std::cout << "published provisioning event " << event_id << std::endl;
            } else {
                const char* values[] = {event_id.c_str()};
                exec_ok(db, "UPDATE outbox_events SET attempts=attempts+1 WHERE id=$1::uuid", 1, values);
                break;
            }
        }
        PQclear(rows);
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

struct OrderRow { std::string id, project_id, state, name, image_ref, flavor_name; };

bool load_order(PGconn* db, const std::string& order_id, OrderRow& order) {
    const char* values[] = {order_id.c_str()};
    PGresult* result = PQexecParams(db,
        "SELECT o.id::text,o.project_id::text,o.state,o.vm_name,i.provider_image_id,f.name "
        "FROM orders o JOIN images i ON i.id=o.image_id JOIN flavors f ON f.id=o.flavor_id WHERE o.id=$1::uuid",
        1, nullptr, values, nullptr, nullptr, 0);
    if (!result || PQresultStatus(result) != PGRES_TUPLES_OK || PQntuples(result) == 0) {
        if (result) PQclear(result);
        return false;
    }
    order = {PQgetvalue(result,0,0), PQgetvalue(result,0,1), PQgetvalue(result,0,2),
             PQgetvalue(result,0,3), PQgetvalue(result,0,4), PQgetvalue(result,0,5)};
    PQclear(result);
    return true;
}

bool transition_to_provisioning(PGconn* db, const OrderRow& order) {
    if (order.state == "PROVISIONING") return true;
    if (order.state != "PENDING") return false;
    if (!exec_ok(db, "BEGIN")) return false;
    const char* values[] = {order.id.c_str()};
    PGresult* update = PQexecParams(db, "UPDATE orders SET state='PROVISIONING',updated_at=now() WHERE id=$1::uuid AND state='PENDING'", 1, nullptr, values, nullptr, nullptr, 0);
    const bool changed = update && PQresultStatus(update) == PGRES_COMMAND_OK && std::string(PQcmdTuples(update)) == "1";
    if (update) PQclear(update);
    if (!changed) { exec_ok(db, "ROLLBACK"); return false; }
    const bool written = exec_ok(db, "INSERT INTO order_events(order_id,from_state,to_state,event_type) VALUES($1::uuid,'PENDING','PROVISIONING','PROVISIONING_STARTED')", 1, values);
    if (!written) { exec_ok(db, "ROLLBACK"); return false; }
    if (!exec_ok(db, "COMMIT")) { exec_ok(db, "ROLLBACK"); return false; }
    return true;
}

bool finish_ready(PGconn* db, const OrderRow& order, const vmaas::ports::VmInfo& vm) {
    if (!exec_ok(db, "BEGIN")) return false;
    const char* values[] = {order.id.c_str(), vm.provider_id.c_str(), order.project_id.c_str(), order.name.c_str(), vm.private_ip.c_str()};
    PGresult* insert = PQexecParams(db,
        "INSERT INTO virtual_machines(order_id,project_id,provider_vm_id,name,state,private_ip) "
        "VALUES($1::uuid,$3,$2,$4,'ACTIVE',NULLIF($5,'')::inet) ON CONFLICT(order_id) DO UPDATE "
        "SET provider_vm_id=EXCLUDED.provider_vm_id,state='ACTIVE',private_ip=EXCLUDED.private_ip",
        5, nullptr, values, nullptr, nullptr, 0);
    const bool inserted = insert && PQresultStatus(insert) == PGRES_COMMAND_OK;
    if (insert) PQclear(insert);
    if (!inserted) { exec_ok(db, "ROLLBACK"); return false; }
    const char* order_values[] = {order.id.c_str()};
    const bool updated = exec_ok(db, "UPDATE orders SET state='READY',updated_at=now(),failure_code=NULL WHERE id=$1::uuid AND state='PROVISIONING'", 1, order_values);
    const bool reserved = exec_ok(db, "UPDATE resource_reservations SET status='COMMITTED' WHERE order_id=$1::uuid AND status='HELD'", 1, order_values);
    const char* event_values[] = {order.id.c_str(), vm.provider_id.c_str()};
    const bool event = exec_ok(db, "INSERT INTO order_events(order_id,from_state,to_state,event_type,details) VALUES($1::uuid,'PROVISIONING','READY','VM_PROVISIONED',jsonb_build_object('providerVmId',$2))", 2, event_values);
    if (!updated || !reserved || !event) { exec_ok(db, "ROLLBACK"); return false; }
    return exec_ok(db, "COMMIT");
}

void finish_failed(PGconn* db, const OrderRow& order, const std::string& reason) {
    if (!exec_ok(db, "BEGIN")) return;
    const std::string short_reason = reason.substr(0, 2000);
    const char* values[] = {order.id.c_str(), short_reason.c_str()};
    const bool failed = exec_ok(db, "UPDATE orders SET state='FAILED',failure_code='PROVIDER_PROVISIONING_FAILED',updated_at=now() WHERE id=$1::uuid AND state='PROVISIONING'", 1, values);
    const bool released = exec_ok(db, "UPDATE resource_reservations SET status='RELEASED',released_at=now() WHERE order_id=$1::uuid AND status='HELD'", 1, values);
    const bool event = exec_ok(db, "INSERT INTO order_events(order_id,from_state,to_state,event_type,details) VALUES($1::uuid,'PROVISIONING','FAILED','PROVISIONING_FAILED',jsonb_build_object('reason',$2))", 2, values);
    if (!failed || !released || !event) { exec_ok(db, "ROLLBACK"); return; }
    exec_ok(db, "COMMIT");
}

bool provision(PGconn* db, vmaas::ports::ComputeProvider& provider, const std::string& order_id, int timeout_seconds) {
    OrderRow order;
    if (!load_order(db, order_id, order)) { std::cerr << "order not found: " << order_id << std::endl; return true; }
    if (order.state == "READY" || order.state == "FAILED" || order.state == "DELETED" || order.state == "CANCELLED") return true;
    if (!transition_to_provisioning(db, order)) {
        OrderRow current;
        if (!load_order(db, order_id, current) || current.state != "PROVISIONING") return true;
        order = current;
    }
    try {
        const vmaas::ports::CreateVmRequest create_request{order.id, order.name, order.image_ref, order.flavor_name};
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
        vmaas::ports::VmInfo vm;
        std::string last_error;
        bool ready = false;
        for (int attempt = 0; attempt < 3 && std::chrono::steady_clock::now() < deadline; ++attempt) {
            try {
                vm = provider.create(create_request);
                while (vm.state != "ACTIVE") {
                    if (vm.state == "ERROR" || vm.state == "DELETED") throw std::runtime_error("provider returned terminal state " + vm.state);
                    if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("timed out waiting for VM to become ACTIVE");
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                    vm = provider.inspect(vm.provider_id);
                }
                ready = true;
                break;
            } catch (const std::exception& error) {
                last_error = error.what();
                if (attempt < 2) std::this_thread::sleep_for(std::chrono::seconds(1 << attempt));
            }
        }
        if (!ready) throw std::runtime_error(last_error.empty() ? "provisioning deadline exceeded" : last_error);
        if (!finish_ready(db, order, vm)) {
            std::cerr << "could not persist the provisioned VM; keeping the Kafka message uncommitted for recovery" << std::endl;
            return false;
        }
        std::cout << "provisioned order " << order.id << " as " << vm.provider_id << std::endl;
        return true;
    } catch (const std::exception& error) {
        std::cerr << "provisioning failed for " << order.id << ": " << error.what() << std::endl;
        finish_failed(db, order, error.what());
        return true;
    }
}
} // namespace

int main() {
    const std::string db_url = env("DATABASE_URL", "postgresql://vmaas:vmaas_dev_password@localhost:5432/vmaas");
    const std::string brokers = env("KAFKA_BOOTSTRAP_SERVERS", "localhost:9092");
    const std::string provider_name = env("COMPUTE_PROVIDER", "mock");
    std::unique_ptr<vmaas::ports::ComputeProvider> provider;
    try { provider = vmaas::mvp::make_compute_provider(provider_name); }
    catch (const std::exception& error) { std::cerr << "provider configuration error: " << error.what() << std::endl; return 1; }
    const int timeout_seconds = std::max(10, std::atoi(env("PROVISIONING_TIMEOUT_SECONDS", "180").c_str()));
    std::thread(outbox_loop, db_url, brokers).detach();

    char error[512];
    rd_kafka_conf_t* config = rd_kafka_conf_new();
    rd_kafka_conf_set(config, "bootstrap.servers", brokers.c_str(), error, sizeof(error));
    rd_kafka_conf_set(config, "group.id", "vmaas-provisioning-v2", error, sizeof(error));
    rd_kafka_conf_set(config, "auto.offset.reset", "earliest", error, sizeof(error));
    rd_kafka_conf_set(config, "enable.auto.commit", "false", error, sizeof(error));
    rd_kafka_t* consumer = rd_kafka_new(RD_KAFKA_CONSUMER, config, error, sizeof(error));
    if (!consumer) { std::cerr << "Kafka consumer init failed: " << error << std::endl; return 1; }
    rd_kafka_poll_set_consumer(consumer);
    rd_kafka_topic_partition_list_t* topics = rd_kafka_topic_partition_list_new(1);
    rd_kafka_topic_partition_list_add(topics, "vm.provision.requested", RD_KAFKA_PARTITION_UA);
    if (rd_kafka_subscribe(consumer, topics) != RD_KAFKA_RESP_ERR_NO_ERROR) {
        std::cerr << "Kafka subscribe failed" << std::endl;
        return 1;
    }
    rd_kafka_topic_partition_list_destroy(topics);
    std::cout << "provisioning worker started with provider " << provider_name << std::endl;

    while (true) {
        rd_kafka_message_t* message = rd_kafka_consumer_poll(consumer, 1000);
        if (!message) continue;
        if (message->err) { rd_kafka_message_destroy(message); continue; }
        bool handled = false;
        std::string order_id;
        try {
            const auto event = json::parse(std::string(static_cast<char*>(message->payload), message->len));
            order_id = event.at("orderId").get<std::string>();
        } catch (const json::exception& exception) {
            std::cerr << "invalid provisioning event (discarded): " << exception.what() << std::endl;
            handled = true;
        }
        while (!handled) {
            try {
                PGconn* db = connect_db(db_url);
                if (db) {
                    handled = provision(db, *provider, order_id, timeout_seconds);
                    PQfinish(db);
                }
            } catch (const std::exception& exception) {
                std::cerr << "could not process provisioning message: " << exception.what() << std::endl;
            }
            if (!handled) std::this_thread::sleep_for(std::chrono::seconds(2));
        }
        if (handled) rd_kafka_commit_message(consumer, message, 0);
        rd_kafka_message_destroy(message);
    }
}
