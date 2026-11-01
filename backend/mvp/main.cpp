#include <algorithm>
#include <arpa/inet.h>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <libpq-fe.h>
#include <map>
#include <mutex>
#include <netinet/in.h>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using json = nlohmann::json;
namespace {
    std::string env(const char* key, const char* fallback) {
        const char* v = std::getenv(key);
        return v ? v : fallback;
    }
    struct Response {
        int status = 200;
        std::string body;
        std::string type = "application/json";
    };
    std::string escape(const std::string& s) {
        std::string o;
        for (char c : s) {
            if (c == '"' || c == '\\')
                o += '\\';
            if (c == '\n')
                o += "\\n";
            else if (c != '\r')
                o += c;
        }
        return o;
    }
    Response error(int status, const std::string& code, const std::string& msg) {
        return { status, json({{"code", code}, {"message", msg}}).dump() };
    }
    PGconn* db = nullptr;
    std::mutex request_mutex;
    PGresult* query(const char* sql, int n = 0,
        const char* const* values = nullptr) {
        return PQexecParams(db, sql, n, nullptr, values, nullptr, nullptr, 0);
    }
    Response handle(const std::string& method, const std::string& path,
        const std::string& body, const std::string& authToken) {
        if (method == "GET" &&
            (path == "/" || path == "/index.html" || path == "/app.js" ||
                path == "/styles.css" || path == "/auth.css")) {
            const std::string file =
                path == "/" || path == "/index.html"
                ? "/usr/local/share/vmaas/frontend/index.html"
                : path == "/app.js" ? "/usr/local/share/vmaas/frontend/app.js"
                : path == "/auth.css" ? "/usr/local/share/vmaas/frontend/auth.css"
                : "/usr/local/share/vmaas/frontend/styles.css";
            std::ifstream input(file, std::ios::binary);
            if (!input)
                return error(404, "NOT_FOUND", "Frontend asset not found");
            std::ostringstream content;
            content << input.rdbuf();
            const std::string type = path == "/app.js"
                ? "application/javascript; charset=utf-8"
                : (path == "/styles.css" || path == "/auth.css")
                ? "text/css; charset=utf-8"
                : "text/html; charset=utf-8";
            return { 200, content.str(), type };
        }
        if (method == "GET" && path == "/health") {
            PGresult* r = PQexec(db, "SELECT 1");
            bool ok = r && PQresultStatus(r) == PGRES_TUPLES_OK;
            if (r)
                PQclear(r);
            return { ok ? 200 : 503,
                    json({{"status", ok ? "ok" : "unavailable"},
                          {"service", "vmaas-api"},
                          {"computeProvider", env("COMPUTE_PROVIDER", "mock")}})
                        .dump() };
        }
        if (method == "POST" &&
            (path == "/api/v1/auth/register" || path == "/api/v1/auth/login")) {
            json in;
            try {
                in = json::parse(body);
            }
            catch (...) {
                return error(400, "INVALID_JSON", "Request body must be valid JSON");
            }
            if (!in.contains("email") || !in.contains("password"))
                return error(400, "VALIDATION_ERROR", "email and password are required");
            const std::string email = in["email"].get<std::string>(),
                password = in["password"].get<std::string>();
            if (email.empty() || email.size() > 254 || password.size() < 8 ||
                password.size() > 128)
                return error(400, "VALIDATION_ERROR",
                    "Email or password length is invalid");
            if (path == "/api/v1/auth/register") {
                if (!in.contains("displayName"))
                    return error(400, "VALIDATION_ERROR", "displayName is required");
                const std::string display = in["displayName"].get<std::string>();
                if (display.empty() || display.size() > 120)
                    return error(400, "VALIDATION_ERROR",
                        "displayName must be 1-120 characters");
                PGresult* begin = PQexec(db, "BEGIN");
                if (begin)
                    PQclear(begin);
                const char* vals[] = { email.c_str(), password.c_str(), display.c_str() };
                PGresult* u = query("INSERT INTO users(email,password_hash,display_name) "
                    "VALUES($1,crypt($2,gen_salt('bf')),$3) RETURNING id",
                    3, vals);
                if (!u || PQresultStatus(u) != PGRES_TUPLES_OK) {
                    if (u)
                        PQclear(u);
                    PQexec(db, "ROLLBACK");
                    return error(
                        409, "REGISTRATION_FAILED",
                        "Email is already registered or account could not be created");
                }
                const std::string uid = PQgetvalue(u, 0, 0);
                PQclear(u);
                const char* uv[] = { uid.c_str() };
                PGresult* role =
                    query("INSERT INTO user_roles(user_id,role_id) SELECT $1,id FROM "
                        "roles WHERE code='USER' ON CONFLICT DO NOTHING",
                        1, uv);
                if (role)
                    PQclear(role);
                const char* pv[] = { email.c_str(), uid.c_str() };
                PGresult* p = query("INSERT INTO projects(name,status,created_by) "
                    "VALUES('Project: '||$1,'ACTIVE',$2) RETURNING id",
                    2, pv);
                if (!p || PQresultStatus(p) != PGRES_TUPLES_OK) {
                    if (p)
                        PQclear(p);
                    PQexec(db, "ROLLBACK");
                    return error(500, "REGISTRATION_FAILED",
                        "Could not create the personal project");
                }
                const std::string pid = PQgetvalue(p, 0, 0);
                PQclear(p);
                const char* mv[] = { pid.c_str(), uid.c_str() };
                PGresult* m =
                    query("INSERT INTO project_members(project_id,user_id,role_code) "
                        "VALUES($1,$2,'OWNER')",
                        2, mv);
                if (m)
                    PQclear(m);
                const char* qv[] = { pid.c_str() };
                PGresult* q = query("INSERT INTO quotas(project_id,resource,limit_value) "
                    "VALUES($1,'VCPU',16),($1,'MEMORY_MB',32768),($1,'"
                    "DISK_GB',500),($1,'INSTANCE',10)",
                    1, qv);
                if (q)
                    PQclear(q);
                PGresult* t = PQexec(db, "SELECT gen_random_uuid()::text");
                if (!t || PQresultStatus(t) != PGRES_TUPLES_OK) {
                    if (t)
                        PQclear(t);
                    PQexec(db, "ROLLBACK");
                    return error(500, "SESSION_FAILED", "Could not create a session");
                }
                const std::string token = PQgetvalue(t, 0, 0);
                PQclear(t);
                const char* tv[] = { token.c_str(), uid.c_str() };
                PGresult* s =
                    query("INSERT INTO refresh_tokens(user_id,token_hash,expires_at) "
                        "VALUES($2,encode(digest($1,'sha256'),'hex'),now()+interval '8 "
                        "hours')",
                        2, tv);
                if (s)
                    PQclear(s);
                PGresult* c = PQexec(db, "COMMIT");
                bool committed = c && PQresultStatus(c) == PGRES_COMMAND_OK;
                if (c)
                    PQclear(c);
                if (!committed)
                    return error(500, "SESSION_FAILED", "Could not save registration");
                return {
                    201,
                    json({{"accessToken", token},
                          {"tokenType", "Bearer"},
                          {"expiresIn", 28800},
                          {"user",
                           {{"id", uid}, {"email", email}, {"displayName", display}}}})
                        .dump() };
            }
            const char* vals[] = { email.c_str(), password.c_str() };
            PGresult* u =
                query("SELECT id,email,display_name FROM users WHERE email=$1 AND "
                    "password_hash=crypt($2,password_hash) AND status='ACTIVE'",
                    2, vals);
            if (!u || PQresultStatus(u) != PGRES_TUPLES_OK) {
                if (u)
                    PQclear(u);
                return error(500, "DB_ERROR", "Could not verify credentials");
            }
            if (!PQntuples(u)) {
                PQclear(u);
                return error(401, "INVALID_CREDENTIALS",
                    "Email or password is incorrect");
            }
            const std::string uid = PQgetvalue(u, 0, 0), display = PQgetvalue(u, 0, 2);
            PQclear(u);
            PGresult* tr = PQexec(db, "SELECT gen_random_uuid()::text");
            if (!tr || PQresultStatus(tr) != PGRES_TUPLES_OK) {
                if (tr)
                    PQclear(tr);
                return error(500, "SESSION_FAILED", "Could not create a session");
            }
            const std::string token = PQgetvalue(tr, 0, 0);
            PQclear(tr);
            const char* tv[] = { token.c_str(), uid.c_str() };
            PGresult* sr = query(
                "INSERT INTO refresh_tokens(user_id,token_hash,expires_at) "
                "VALUES($2,encode(digest($1,'sha256'),'hex'),now()+interval '8 hours')",
                2, tv);
            if (!sr || PQresultStatus(sr) != PGRES_COMMAND_OK) {
                if (sr)
                    PQclear(sr);
                return error(500, "SESSION_FAILED", "Could not save session");
            }
            PQclear(sr);
            return { 200,
                    json({{"accessToken", token},
                          {"tokenType", "Bearer"},
                          {"expiresIn", 28800},
                          {"user",
                           {{"id", uid}, {"email", email}, {"displayName", display}}}})
                        .dump() };
        }
        std::string userId;
        if (!authToken.empty()) {
            const char* tv[] = { authToken.c_str() };
            PGresult* s = query(
                "SELECT u.id::text FROM users u JOIN refresh_tokens t ON "
                "t.user_id=u.id WHERE t.token_hash=encode(digest($1,'sha256'),'hex') "
                "AND t.revoked_at IS NULL AND t.expires_at>now() AND u.status='ACTIVE'",
                1, tv);
            if (s && PQresultStatus(s) == PGRES_TUPLES_OK && PQntuples(s))
                userId = PQgetvalue(s, 0, 0);
            if (s)
                PQclear(s);
        }
        if (method == "GET" && path == "/api/v1/auth/me") {
            if (userId.empty())
                return error(401, "UNAUTHORIZED", "Sign in to continue");
            const char* uv[] = { userId.c_str() };
            PGresult* u = query(
                "SELECT id::text,email,display_name FROM users WHERE id=$1", 1, uv);
            if (!u || PQresultStatus(u) != PGRES_TUPLES_OK || !PQntuples(u)) {
                if (u)
                    PQclear(u);
                return error(401, "UNAUTHORIZED", "Session is not valid");
            }
            json result = { {"id", PQgetvalue(u, 0, 0)},
                           {"email", PQgetvalue(u, 0, 1)},
                           {"displayName", PQgetvalue(u, 0, 2)} };
            PQclear(u);
            return { 200, json({{"user", result}}).dump() };
        }
        if (method == "POST" && path == "/api/v1/auth/refresh") {
            if (userId.empty())
                return error(401, "UNAUTHORIZED", "Session is not valid");
            PGresult* tr = PQexec(db, "SELECT gen_random_uuid()::text");
            if (!tr || PQresultStatus(tr) != PGRES_TUPLES_OK) {
                if (tr)
                    PQclear(tr);
                return error(500, "SESSION_FAILED", "Could not refresh session");
            }
            const std::string token = PQgetvalue(tr, 0, 0);
            PQclear(tr);
            const char* old[] = { authToken.c_str() };
            PGresult* revoke = query(
                "UPDATE refresh_tokens SET revoked_at=now() WHERE "
                "token_hash=encode(digest($1,'sha256'),'hex') AND revoked_at IS NULL",
                1, old);
            if (revoke)
                PQclear(revoke);
            const char* next[] = { token.c_str(), userId.c_str() };
            PGresult* save = query(
                "INSERT INTO refresh_tokens(user_id,token_hash,expires_at) "
                "VALUES($2,encode(digest($1,'sha256'),'hex'),now()+interval '8 hours')",
                2, next);
            if (!save || PQresultStatus(save) != PGRES_COMMAND_OK) {
                if (save)
                    PQclear(save);
                return error(500, "SESSION_FAILED", "Could not refresh session");
            }
            PQclear(save);
            return { 200, json({{"accessToken", token},
                               {"tokenType", "Bearer"},
                               {"expiresIn", 28800}})
                             .dump() };
        }
        if (method == "POST" && path == "/api/v1/auth/logout") {
            if (authToken.empty())
                return error(401, "UNAUTHORIZED", "Sign in to continue");
            const char* tv[] = { authToken.c_str() };
            PGresult* r = query(
                "UPDATE refresh_tokens SET revoked_at=now() WHERE "
                "token_hash=encode(digest($1,'sha256'),'hex') AND revoked_at IS NULL",
                1, tv);
            if (r)
                PQclear(r);
            return { 200, json({{"status", "ok"}}).dump() };
        }
        if (method == "GET" && path == "/api/v1/runtime")
            return {
                200,
                json({{"computeProvider", env("COMPUTE_PROVIDER", "mock")}}).dump() };
        if (path.rfind("/api/v1/", 0) == 0 &&
            path.rfind("/api/v1/catalog/", 0) != 0 && path != "/api/v1/runtime" &&
            userId.empty())
            return error(401, "UNAUTHORIZED", "Sign in to continue");
        if (method == "GET" && path == "/api/v1/catalog/flavors") {
            PGresult* r = PQexec(db, "SELECT id,name,vcpu,memory_mb,disk_gb FROM "
                "flavors WHERE active ORDER BY id");
            if (!r || PQresultStatus(r) != PGRES_TUPLES_OK) {
                if (r)
                    PQclear(r);
                return error(500, "DB_ERROR", "Could not load flavor catalog");
            }
            json a = json::array();
            for (int i = 0; i < PQntuples(r); ++i)
                a.push_back({ {"id", PQgetvalue(r, i, 0)},
                             {"name", PQgetvalue(r, i, 1)},
                             {"vcpu", atoi(PQgetvalue(r, i, 2))},
                             {"memoryMb", atoi(PQgetvalue(r, i, 3))},
                             {"diskGb", atoi(PQgetvalue(r, i, 4))} });
            PQclear(r);
            return { 200, json({{"items", a}}).dump() };
        }
        if (method == "GET" && path == "/api/v1/catalog/images") {
            PGresult* r = PQexec(db, "SELECT id,name,os_family,version FROM images "
                "WHERE active ORDER BY id");
            if (!r || PQresultStatus(r) != PGRES_TUPLES_OK) {
                if (r)
                    PQclear(r);
                return error(500, "DB_ERROR", "Could not load image catalog");
            }
            json a = json::array();
            for (int i = 0; i < PQntuples(r); ++i)
                a.push_back({ {"id", atoi(PQgetvalue(r, i, 0))},
                             {"name", PQgetvalue(r, i, 1)},
                             {"osFamily", PQgetvalue(r, i, 2)},
                             {"version", PQgetvalue(r, i, 3)} });
            PQclear(r);
            return { 200, json({{"items", a}}).dump() };
        }
        if (method == "GET" && path == "/api/v1/projects") {
            const char* v[] = { userId.c_str() };
            PGresult* r = query("SELECT p.id,p.name,pm.role_code FROM projects p JOIN "
                "project_members pm ON pm.project_id=p.id WHERE "
                "pm.user_id=$1 AND p.status='ACTIVE' ORDER BY p.name",
                1, v);
            if (!r || PQresultStatus(r) != PGRES_TUPLES_OK) {
                if (r)
                    PQclear(r);
                return error(500, "DB_ERROR", "Could not load projects");
            }
            json a = json::array();
            for (int i = 0; i < PQntuples(r); ++i)
                a.push_back({ {"id", atoi(PQgetvalue(r, i, 0))},
                             {"name", PQgetvalue(r, i, 1)},
                             {"role", PQgetvalue(r, i, 2)} });
            PQclear(r);
            return { 200, json({{"items", a}}).dump() };
        }
        if (method == "GET" && path == "/api/v1/orders") {
            const char* v[] = { userId.c_str() };
            PGresult* r =
                query("SELECT o.id,o.vm_name,o.state,o.created_at::text,f.name,i.name "
                    "FROM orders o JOIN project_members pm ON "
                    "pm.project_id=o.project_id AND pm.user_id=o.requested_by JOIN "
                    "flavors f ON f.id=o.flavor_id JOIN images i ON i.id=o.image_id "
                    "WHERE o.requested_by=$1 ORDER BY o.created_at DESC LIMIT 100",
                    1, v);
            if (!r || PQresultStatus(r) != PGRES_TUPLES_OK) {
                if (r)
                    PQclear(r);
                return error(500, "DB_ERROR", "Could not load orders");
            }
            json a = json::array();
            for (int i = 0; i < PQntuples(r); ++i)
                a.push_back({ {"orderId", PQgetvalue(r, i, 0)},
                             {"vmName", PQgetvalue(r, i, 1)},
                             {"state", PQgetvalue(r, i, 2)},
                             {"createdAt", PQgetvalue(r, i, 3)},
                             {"flavor", PQgetvalue(r, i, 4)},
                             {"image", PQgetvalue(r, i, 5)} });
            PQclear(r);
            return { 200, json({{"items", a}}).dump() };
        }
        if (method == "POST" && path == "/api/v1/orders") {
            json in;
            try {
                in = json::parse(body);
            }
            catch (...) {
                return error(400, "INVALID_JSON", "Request body must be valid JSON");
            }
            for (const char* k :
                { "projectId", "imageId", "flavorId", "vmName", "idempotencyKey" })
                if (!in.contains(k))
                    return error(400, "VALIDATION_ERROR",
                        std::string("Missing field: ") + k);
            const std::string project =
                in["projectId"].is_string()
                ? in["projectId"].get<std::string>()
                : std::to_string(in["projectId"].get<long long>());
            const std::string image =
                in["imageId"].is_string()
                ? in["imageId"].get<std::string>()
                : std::to_string(in["imageId"].get<long long>());
            const std::string flavor =
                in["flavorId"].is_string()
                ? in["flavorId"].get<std::string>()
                : std::to_string(in["flavorId"].get<long long>());
            std::string name = in["vmName"].get<std::string>();
            std::string key = in["idempotencyKey"].get<std::string>();
            if (name.empty() || name.size() > 63 || key.empty() || key.size() > 128)
                return error(400, "VALIDATION_ERROR",
                    "vmName must be 1-63 chars and idempotencyKey 1-128 chars");
            PGresult* begin = PQexec(db, "BEGIN");
            if (!begin || PQresultStatus(begin) != PGRES_COMMAND_OK) {
                if (begin)
                    PQclear(begin);
                return error(500, "DB_ERROR", "Could not begin order transaction");
            }
            PQclear(begin);
            const char* access[] = { project.c_str(), image.c_str(), flavor.c_str(),
                                    userId.c_str() };
            PGresult* check =
                query("SELECT f.vcpu,f.memory_mb,f.disk_gb FROM project_members pm "
                    "JOIN projects p ON p.id=pm.project_id JOIN images i ON i.id=$2 "
                    "AND i.active JOIN flavors f ON f.id=$3 AND f.active WHERE "
                    "pm.project_id=$1 AND pm.user_id=$4 AND pm.role_code<>'VIEWER' "
                    "AND p.status='ACTIVE' FOR UPDATE OF p",
                    4, access);
            if (!check || PQresultStatus(check) != PGRES_TUPLES_OK) {
                if (check)
                    PQclear(check);
                PQexec(db, "ROLLBACK");
                return error(500, "DB_ERROR",
                    "Could not validate project membership and catalog");
            }
            if (!PQntuples(check)) {
                PQclear(check);
                PQexec(db, "ROLLBACK");
                return error(422, "ORDER_NOT_ALLOWED",
                    "Project, membership, image or flavor is invalid");
            }
            const long long vcpu = std::atoll(PQgetvalue(check, 0, 0)),
                memory = std::atoll(PQgetvalue(check, 0, 1)),
                disk = std::atoll(PQgetvalue(check, 0, 2));
            PQclear(check);
            const char* idem[] = { project.c_str(), key.c_str(), userId.c_str() };
            PGresult* prior = query("SELECT id,state FROM orders WHERE project_id=$1 "
                "AND idempotency_key=$2 AND requested_by=$3",
                3, idem);
            if (prior && PQresultStatus(prior) == PGRES_TUPLES_OK &&
                PQntuples(prior) > 0) {
                json out = { {"orderId", PQgetvalue(prior, 0, 0)},
                            {"state", PQgetvalue(prior, 0, 1)},
                            {"idempotentReplay", true} };
                PQclear(prior);
                PQexec(db, "COMMIT");
                return { 200, out.dump() };
            }
            if (prior)
                PQclear(prior);
            const char* qv[] = { project.c_str() };
            PGresult* quota = query("SELECT resource,limit_value FROM quotas WHERE "
                "project_id=$1 FOR UPDATE",
                1, qv);
            if (!quota || PQresultStatus(quota) != PGRES_TUPLES_OK) {
                if (quota)
                    PQclear(quota);
                PQexec(db, "ROLLBACK");
                return error(500, "DB_ERROR", "Could not load project quotas");
            }
            std::map<std::string, long long> limits;
            for (int i = 0; i < PQntuples(quota); ++i)
                limits[PQgetvalue(quota, i, 0)] = std::atoll(PQgetvalue(quota, i, 1));
            PQclear(quota);
            PGresult* usage = query("SELECT resource,COALESCE(sum(amount),0) FROM "
                "resource_reservations WHERE project_id=$1 AND "
                "status IN ('HELD','COMMITTED') GROUP BY resource",
                1, qv);
            if (!usage || PQresultStatus(usage) != PGRES_TUPLES_OK) {
                if (usage)
                    PQclear(usage);
                PQexec(db, "ROLLBACK");
                return error(500, "DB_ERROR", "Could not calculate reserved resources");
            }
            std::map<std::string, long long> used;
            for (int i = 0; i < PQntuples(usage); ++i)
                used[PQgetvalue(usage, i, 0)] = std::atoll(PQgetvalue(usage, i, 1));
            PQclear(usage);
            const std::map<std::string, long long> requested = { {"VCPU", vcpu},
                                                                {"MEMORY_MB", memory},
                                                                {"DISK_GB", disk},
                                                                {"INSTANCE", 1} };
            for (const auto& [resource, amount] : requested) {
                if (!limits.contains(resource) ||
                    used[resource] + amount > limits[resource]) {
                    PQexec(db, "ROLLBACK");
                    return error(422, "QUOTA_EXCEEDED",
                        "Project quota exceeded for resource: " + resource);
                }
            }
            const char* vals[] = { project.c_str(), userId.c_str(), image.c_str(),
                                  flavor.c_str(),  name.c_str(),   key.c_str() };
            PGresult* r = query("INSERT INTO "
                "orders(id,project_id,requested_by,image_id,flavor_id,"
                "vm_name,state,idempotency_key,correlation_id) "
                "VALUES(gen_random_uuid(),$1,$2,$3,$4,$5,'PENDING',$6,"
                "gen_random_uuid()) RETURNING id,correlation_id",
                6, vals);
            if (!r || PQresultStatus(r) != PGRES_TUPLES_OK) {
                if (r)
                    PQclear(r);
                PQexec(db, "ROLLBACK");
                return error(
                    409, "ORDER_CREATE_FAILED",
                    "Could not create order (possibly duplicate idempotency key)");
            }
            const std::string id = PQgetvalue(r, 0, 0), corr = PQgetvalue(r, 0, 1);
            PQclear(r);
            const std::string vcpu_text = std::to_string(vcpu),
                memory_text = std::to_string(memory),
                disk_text = std::to_string(disk);
            const char* rv[] = { id.c_str(), project.c_str(), vcpu_text.c_str(),
                                memory_text.c_str(), disk_text.c_str() };
            PGresult* reservations =
                query("INSERT INTO "
                    "resource_reservations(order_id,project_id,resource,amount) "
                    "VALUES($1,$2,'VCPU',$3),($1,$2,'MEMORY_MB',$4),($1,$2,'DISK_GB',"
                    "$5),($1,$2,'INSTANCE',1)",
                    5, rv);
            if (!reservations || PQresultStatus(reservations) != PGRES_COMMAND_OK) {
                if (reservations)
                    PQclear(reservations);
                PQexec(db, "ROLLBACK");
                return error(500, "RESERVATION_FAILED",
                    "Could not reserve project resources");
            }
            PQclear(reservations);
            const char* ev[] = { id.c_str() };
            PGresult* history =
                query("INSERT INTO order_events(order_id,to_state,event_type) "
                    "VALUES($1,'PENDING','ORDER_CREATED')",
                    1, ev);
            if (!history || PQresultStatus(history) != PGRES_COMMAND_OK) {
                if (history)
                    PQclear(history);
                PQexec(db, "ROLLBACK");
                return error(500, "DB_ERROR", "Could not write order history");
            }
            PQclear(history);
            PGresult* event_id = PQexec(db, "SELECT gen_random_uuid()::text");
            if (!event_id || PQresultStatus(event_id) != PGRES_TUPLES_OK) {
                if (event_id)
                    PQclear(event_id);
                PQexec(db, "ROLLBACK");
                return error(500, "DB_ERROR", "Could not allocate event ID");
            }
            const std::string eventId = PQgetvalue(event_id, 0, 0);
            PQclear(event_id);
            json payload = { {"eventId", eventId},
                            {"eventType", "vm.provision.requested"},
                            {"schemaVersion", 1},
                            {"orderId", id},
                            {"correlationId", corr} };
            const std::string payload_text = payload.dump();
            const char* outbox[] = { eventId.c_str(), id.c_str(), payload_text.c_str() };
            PGresult* out =
                query("INSERT INTO "
                    "outbox_events(id,aggregate_type,aggregate_id,event_type,schema_"
                    "version,payload) "
                    "VALUES($1,'ORDER',$2,'vm.provision.requested',1,$3::jsonb)",
                    3, outbox);
            if (!out || PQresultStatus(out) != PGRES_COMMAND_OK) {
                if (out)
                    PQclear(out);
                PQexec(db, "ROLLBACK");
                return error(500, "OUTBOX_FAILED",
                    "Could not queue provisioning command");
            }
            PQclear(out);
            PGresult* commit = PQexec(db, "COMMIT");
            const bool committed = commit && PQresultStatus(commit) == PGRES_COMMAND_OK;
            if (commit)
                PQclear(commit);
            if (!committed)
                return error(500, "DB_COMMIT_FAILED", "Order transaction failed");
            return { 202, json({{"orderId", id},
                               {"correlationId", corr},
                               {"state", "PENDING"},
                               {"outboxQueued", true}})
                             .dump() };
        }
        if (method == "GET" && path.rfind("/api/v1/orders/", 0) == 0) {
            std::string id = path.substr(15);
            const char* v[] = { id.c_str(), userId.c_str() };
            PGresult* r = query(
                "SELECT "
                "o.id,o.state,o.vm_name,o.created_at::text,f.name,f.vcpu,f.memory_mb,f."
                "disk_gb,i.name,COALESCE(vm.provider_vm_id,''),COALESCE(host(vm."
                "private_ip),'') FROM orders o JOIN flavors f ON f.id=o.flavor_id JOIN "
                "images i ON i.id=o.image_id LEFT JOIN virtual_machines vm ON "
                "vm.order_id=o.id WHERE o.id=$1 AND o.requested_by=$2",
                2, v);
            if (!r || PQresultStatus(r) != PGRES_TUPLES_OK) {
                if (r)
                    PQclear(r);
                return error(500, "DB_ERROR", "Could not load order");
            }
            if (!PQntuples(r)) {
                PQclear(r);
                return error(404, "NOT_FOUND", "Order not found");
            }
            json out = { {"orderId", PQgetvalue(r, 0, 0)},
                        {"state", PQgetvalue(r, 0, 1)},
                        {"vmName", PQgetvalue(r, 0, 2)},
                        {"createdAt", PQgetvalue(r, 0, 3)},
                        {"flavor",
                         {{"name", PQgetvalue(r, 0, 4)},
                          {"vcpu", atoi(PQgetvalue(r, 0, 5))},
                          {"memoryMb", atoi(PQgetvalue(r, 0, 6))},
                          {"diskGb", atoi(PQgetvalue(r, 0, 7))}}},
                        {"image", PQgetvalue(r, 0, 8)},
                        {"providerVmId", PQgetvalue(r, 0, 9)},
                        {"privateIp", PQgetvalue(r, 0, 10)} };
            PQclear(r);
            return { 200, out.dump() };
        }
        return error(404, "NOT_FOUND", "Route not found");
    }
    void serve(int fd) {
        std::string raw;
        char buf[4096];
        size_t expected = 0, headerEnd = std::string::npos;
        while (raw.size() < 1024 * 1024) {
            ssize_t n = recv(fd, buf, sizeof(buf), 0);
            if (n <= 0)
                return;
            raw.append(buf, n);
            headerEnd = raw.find("\r\n\r\n");
            if (headerEnd != std::string::npos) {
                auto h = raw.substr(0, headerEnd);
                auto pos = h.find("Content-Length:");
                if (pos != std::string::npos)
                    expected = static_cast<size_t>(std::stoul(h.substr(pos + 15)));
                if (raw.size() >= headerEnd + 4 + expected)
                    break;
            }
        }
        if (headerEnd == std::string::npos)
            return;
        std::istringstream hs(raw.substr(0, raw.find("\r\n")));
        std::string method, path, ver;
        hs >> method >> path >> ver;
        try {
            while (raw.size() < headerEnd + 4 + expected) {
                ssize_t n = recv(fd, buf, sizeof(buf), 0);
                if (n <= 0)
                    break;
                raw.append(buf, n);
            }
        }
        catch (...) {
        }
        std::string auth;
        const std::string headerBlock = raw.substr(0, headerEnd);
        const auto authPos = headerBlock.find("Authorization: Bearer ");
        if (authPos != std::string::npos) {
            auto end = headerBlock.find("\r\n", authPos);
            auth = headerBlock.substr(authPos + 22, end == std::string::npos
                ? std::string::npos
                : end - authPos - 22);
        }
        std::string body = raw.substr(headerEnd + 4, expected);
        Response res;
        try {
            std::lock_guard<std::mutex> guard(request_mutex);
            res = handle(method, path, body, auth);
        }
        catch (const std::exception&) {
            res = error(500, "INTERNAL_ERROR", "Unexpected server error");
        }
        const char* reason = res.status == 200 ? "OK"
            : res.status == 201 ? "Created"
            : res.status == 202 ? "Accepted"
            : res.status == 400 ? "Bad Request"
            : res.status == 401 ? "Unauthorized"
            : res.status == 404 ? "Not Found"
            : res.status == 422 ? "Unprocessable Entity"
            : res.status == 409 ? "Conflict"
            : res.status == 503 ? "Service Unavailable"
            : "Internal Server Error";
        std::ostringstream out;
        out << "HTTP/1.1 " << res.status << " " << reason
            << "\r\nContent-Type: " << res.type
            << "\r\nContent-Length: " << res.body.size()
            << "\r\nAccess-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: "
            "GET, POST, OPTIONS\r\nAccess-Control-Allow-Headers: Content-Type, "
            "Idempotency-Key, Authorization\r\nConnection: close\r\n\r\n"
            << res.body;
        std::string response = out.str();
        send(fd, response.data(), response.size(), 0);
    }
} 

int main() {
    const std::string dburl =
        env("DATABASE_URL",
            "postgresql://vmaas:vmaas_dev_password@localhost:5432/vmaas");
    for (int attempt = 0; attempt < 40; ++attempt) {
        db = PQconnectdb(dburl.c_str());
        if (PQstatus(db) == CONNECTION_OK)
            break;
        PQfinish(db);
        db = nullptr;
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    if (!db) {
        std::cerr << "database connection failed\n";
        return 1;
    }
    PGresult* extensions =
        PQexec(db, "CREATE EXTENSION IF NOT EXISTS pgcrypto; UPDATE users SET "
            "password_hash=crypt('Demo1234!',gen_salt('bf')) WHERE id=1 "
            "AND password_hash LIKE '%placeholder%'");
    if (!extensions || PQresultStatus(extensions) != PGRES_COMMAND_OK) {
        std::cerr << "identity initialization failed: " << PQerrorMessage(db)
            << "\n";
        if (extensions)
            PQclear(extensions);
        return 1;
    }
    PQclear(extensions);
    PGresult* catalog =
        PQexec(db, "INSERT INTO flavors(id,name,vcpu,memory_mb,disk_gb,active) "
            "OVERRIDING SYSTEM VALUE VALUES "
            "(1,'small',2,4096,40,true),(2,'medium',4,8192,80,true),(3,'"
            "large',6,12288,120,true),(4,'compute',8,16384,160,true),(5,'"
            "memory',8,32768,200,true) ON CONFLICT(id) DO UPDATE SET "
            "name=EXCLUDED.name,vcpu=EXCLUDED.vcpu,memory_mb=EXCLUDED."
            "memory_mb,disk_gb=EXCLUDED.disk_gb,active=true");
    if (!catalog || PQresultStatus(catalog) != PGRES_COMMAND_OK) {
        std::cerr << "catalog initialization failed: " << PQerrorMessage(db)
            << "\n";
        if (catalog)
            PQclear(catalog);
        return 1;
    }
    PQclear(catalog);
    PGresult* backfill = PQexec(
        db, "INSERT INTO "
        "resource_reservations(order_id,project_id,resource,amount,status) "
        "SELECT o.id,o.project_id,'VCPU',f.vcpu,CASE WHEN o.state IN "
        "('PENDING','PROVISIONING') THEN 'HELD' ELSE 'COMMITTED' END FROM "
        "orders o JOIN flavors f ON f.id=o.flavor_id WHERE o.state IN "
        "('PENDING','PROVISIONING','READY') UNION ALL SELECT "
        "o.id,o.project_id,'MEMORY_MB',f.memory_mb,CASE WHEN o.state IN "
        "('PENDING','PROVISIONING') THEN 'HELD' ELSE 'COMMITTED' END FROM "
        "orders o JOIN flavors f ON f.id=o.flavor_id WHERE o.state IN "
        "('PENDING','PROVISIONING','READY') UNION ALL SELECT "
        "o.id,o.project_id,'DISK_GB',f.disk_gb,CASE WHEN o.state IN "
        "('PENDING','PROVISIONING') THEN 'HELD' ELSE 'COMMITTED' END FROM "
        "orders o JOIN flavors f ON f.id=o.flavor_id WHERE o.state IN "
        "('PENDING','PROVISIONING','READY') UNION ALL SELECT "
        "o.id,o.project_id,'INSTANCE',1,CASE WHEN o.state IN "
        "('PENDING','PROVISIONING') THEN 'HELD' ELSE 'COMMITTED' END FROM "
        "orders o WHERE o.state IN ('PENDING','PROVISIONING','READY') ON "
        "CONFLICT(order_id,resource) DO NOTHING");
    if (!backfill || PQresultStatus(backfill) != PGRES_COMMAND_OK) {
        std::cerr << "reservation backfill failed: " << PQerrorMessage(db) << "\n";
        if (backfill)
            PQclear(backfill);
        return 1;
    }
    PQclear(backfill);
    int port = std::atoi(env("PORT", "8080").c_str());
    int server = socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (bind(server, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
        listen(server, 64) < 0) {
        perror("listen");
        return 1;
    }
    std::cout << "VMaaS API listening on " << port << std::endl;
    while (true) {
        int fd = accept(server, nullptr, nullptr);
        if (fd < 0)
            continue;
        std::thread(serve, fd).detach();
    }
}