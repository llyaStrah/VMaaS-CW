#include "compute_provider.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

using json = nlohmann::json;

namespace vmaas::mvp {
namespace {
std::string env(const char* key, const char* fallback = "") {
    const char* value = std::getenv(key);
    return value ? value : fallback;
}

std::string trim_slash(std::string value) {
    while (!value.empty() && value.back() == '/') value.pop_back();
    return value;
}

struct HttpResult {
    long status{};
    std::string body;
    std::string subject_token;
};

size_t receive_body(char* data, size_t size, size_t count, void* target) {
    const auto bytes = size * count;
    static_cast<std::string*>(target)->append(data, bytes);
    return bytes;
}

size_t receive_header(char* data, size_t size, size_t count, void* target) {
    const auto bytes = size * count;
    std::string line(data, bytes);
    constexpr char token_header[] = "x-subject-token:";
    std::string lower = line;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower.rfind(token_header, 0) == 0) {
        auto first = line.find(':') + 1;
        auto last = line.find_last_not_of("\r\n \t");
        static_cast<HttpResult*>(target)->subject_token = line.substr(first, last - first + 1);
    }
    return bytes;
}

HttpResult request(const std::string& method, const std::string& url,
                   const std::string& token, const std::string& body = {}) {
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("Could not initialize HTTP client");
    HttpResult result;
    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Accept: application/json");
    if (!token.empty()) {
        const std::string auth = "X-Auth-Token: " + token;
        headers = curl_slist_append(headers, auth.c_str());
    }
    if (method == "POST") headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result.body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, receive_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &result);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    if (method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    } else if (method == "DELETE") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    }
    const auto code = curl_easy_perform(curl);
    if (code != CURLE_OK) {
        const std::string message = curl_easy_strerror(code);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        throw std::runtime_error("OpenStack request failed: " + message);
    }
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return result;
}

json parse_response(const HttpResult& response) {
    try {
        return response.body.empty() ? json::object() : json::parse(response.body);
    } catch (...) {
        return json::object();
    }
}

void require_success(const HttpResult& response, const std::string& operation,
                     std::initializer_list<long> accepted) {
    if (std::find(accepted.begin(), accepted.end(), response.status) != accepted.end()) return;
    std::string message;
    const auto data = parse_response(response);
    if (data.contains("fault") && data["fault"].is_object()) {
        const auto& fault = data["fault"];
        if (fault.contains("message") && fault["message"].is_string()) message = fault["message"].get<std::string>();
    } else if (data.contains("message") && data["message"].is_string()) {
        message = data["message"].get<std::string>();
    }
    throw std::runtime_error(operation + " failed with HTTP " + std::to_string(response.status) +
                             (message.empty() ? "" : ": " + message));
}

class MockProvider final : public ports::ComputeProvider {
public:
    ports::VmInfo create(const ports::CreateVmRequest& request) override {
        return {"mock-" + request.order_id, "ACTIVE", ""};
    }
    ports::VmInfo inspect(const std::string& id) override {
        return {id, "ACTIVE", ""};
    }
    void start(const std::string&) override {}
    void stop(const std::string&) override {}
    void reboot(const std::string&) override {}
    void remove(const std::string&) override {}
};

class OpenStackProvider final : public ports::ComputeProvider {
public:
    OpenStackProvider() {
        auth_url_ = trim_slash(env("OPENSTACK_AUTH_URL"));
        username_ = env("OPENSTACK_USERNAME");
        password_ = env("OPENSTACK_PASSWORD");
        project_name_ = env("OPENSTACK_PROJECT_NAME");
        network_id_ = env("OPENSTACK_NETWORK_ID");
        if (auth_url_.empty() || username_.empty() || password_.empty() || project_name_.empty() || network_id_.empty())
            throw std::runtime_error("OpenStack provider requires AUTH_URL, USERNAME, PASSWORD, PROJECT_NAME and NETWORK_ID");
        user_domain_ = env("OPENSTACK_USER_DOMAIN", "Default");
        project_domain_ = env("OPENSTACK_PROJECT_DOMAIN", "Default");
        region_ = env("OPENSTACK_REGION", "RegionOne");
        interface_ = env("OPENSTACK_INTERFACE", "public");
        key_name_ = env("OPENSTACK_KEY_NAME");
        curl_global_init(CURL_GLOBAL_DEFAULT);
    }

    ports::VmInfo create(const ports::CreateVmRequest& input) override {
        const auto flavor = flavor_id(input.flavor_name);
        const auto image = env("OPENSTACK_IMAGE_ID", input.image_provider_id.c_str());
        if (image.empty()) throw std::runtime_error("OpenStack image ID is empty");

        // Resolve an earlier create attempt by order metadata before submitting another server.
        CURL* esc = curl_easy_init();
        char* encoded_name = esc ? curl_easy_escape(esc, input.name.c_str(), static_cast<int>(input.name.size())) : nullptr;
        if (!encoded_name) {
            if (esc) curl_easy_cleanup(esc);
            throw std::runtime_error("Could not encode VM name");
        }
        const std::string list_url = compute_url_ + "/servers/detail?name=" + encoded_name;
        curl_free(encoded_name);
        curl_easy_cleanup(esc);
        auto existing = request("GET", list_url, token());
        require_success(existing, "Find existing server", {200});
        const auto list = parse_response(existing);
        if (list.contains("servers") && list["servers"].is_array()) {
            for (const auto& server : list["servers"]) {
                if (server.value("name", "") == input.name && server.contains("metadata") &&
                    server["metadata"].value("vmaas_order_id", "") == input.order_id) {
                    return server_info(server);
                }
            }
        }

        json server = {{"name", input.name}, {"imageRef", image}, {"flavorRef", flavor},
                       {"networks", json::array({json{{"uuid", network_id_}}})},
                       {"metadata", {{"vmaas_order_id", input.order_id}}}};
        if (!key_name_.empty()) server["key_name"] = key_name_;
        const auto response = request("POST", compute_url_ + "/servers", token(), json{{"server", server}}.dump());
        require_success(response, "Create server", {200, 202});
        const auto data = parse_response(response);
        if (!data.contains("server") || !data["server"].contains("id"))
            throw std::runtime_error("OpenStack did not return the new server ID");
        return server_info(data["server"]);
    }

    ports::VmInfo inspect(const std::string& id) override {
        const auto response = request("GET", compute_url_ + "/servers/" + id, token());
        require_success(response, "Inspect server", {200});
        const auto data = parse_response(response);
        if (!data.contains("server")) throw std::runtime_error("OpenStack response has no server object");
        return server_info(data["server"]);
    }

    void start(const std::string& id) override { action(id, json{{"os-start", nullptr}}); }
    void stop(const std::string& id) override { action(id, json{{"os-stop", nullptr}}); }
    void reboot(const std::string& id) override { action(id, json{{"reboot", {{"type", "SOFT"}}}}); }
    void remove(const std::string& id) override {
        const auto response = request("DELETE", compute_url_ + "/servers/" + id, token());
        require_success(response, "Delete server", {202, 204, 404});
    }

private:
    std::string auth_url_, username_, password_, project_name_, user_domain_, project_domain_;
    std::string network_id_, key_name_, region_, interface_, token_, compute_url_;
    std::chrono::steady_clock::time_point token_expiry_{};

    std::string flavor_id(const std::string& name) const {
        std::string key = "OPENSTACK_FLAVOR_ID_";
        for (const unsigned char c : name) key += static_cast<char>(std::toupper(c));
        const auto value = env(key.c_str());
        if (value.empty()) throw std::runtime_error("Missing OpenStack flavor mapping: " + key);
        return value;
    }

    void authenticate() {
        const std::string endpoint = auth_url_.size() >= 3 && auth_url_.substr(auth_url_.size() - 3) == "/v3"
            ? auth_url_ + "/auth/tokens" : auth_url_ + "/v3/auth/tokens";
        json project_scope = {{"name", project_name_}, {"domain", {{"name", project_domain_}}}};
        const std::string project_id = env("OPENSTACK_PROJECT_ID");
        if (!project_id.empty()) project_scope = {{"id", project_id}};
        const auto body = json{{"auth", {
            {"identity", {{"methods", json::array({"password"})},
                           {"password", {{"user", {{"name", username_}, {"domain", {{"name", user_domain_}}}, {"password", password_}}}}}}},
            {"scope", {{"project", project_scope}}}}}}.dump();
        const auto response = request("POST", endpoint, "", body);
        require_success(response, "OpenStack authentication", {201});
        if (response.subject_token.empty()) throw std::runtime_error("Keystone response did not include X-Subject-Token");
        const auto data = parse_response(response);
        if (!data.contains("token") || !data["token"].contains("catalog"))
            throw std::runtime_error("Keystone response has no service catalog");
        std::string endpoint_url;
        for (const auto& service : data["token"]["catalog"]) {
            if (service.value("type", "") != "compute" || !service.contains("endpoints")) continue;
            for (const auto& item : service["endpoints"]) {
                if (item.value("interface", "") == interface_ && item.value("region", region_) == region_) {
                    endpoint_url = item.value("url", "");
                    if (!endpoint_url.empty()) break;
                }
            }
            if (!endpoint_url.empty()) break;
        }
        if (endpoint_url.empty()) throw std::runtime_error("Keystone catalog has no Compute endpoint for the configured region/interface");
        token_ = response.subject_token;
        compute_url_ = trim_slash(endpoint_url);
        token_expiry_ = std::chrono::steady_clock::now() + std::chrono::minutes(45);
    }

    const std::string& token() {
        if (token_.empty() || std::chrono::steady_clock::now() >= token_expiry_) authenticate();
        return token_;
    }

    ports::VmInfo server_info(const json& server) const {
        std::string ip;
        if (server.contains("addresses") && server["addresses"].is_object()) {
            for (auto it = server["addresses"].begin(); it != server["addresses"].end() && ip.empty(); ++it) {
                if (!it.value().is_array()) continue;
                for (const auto& address : it.value()) {
                    if (address.value("OS-EXT-IPS:type", "fixed") == "fixed" && address.contains("addr")) {
                        ip = address["addr"].get<std::string>();
                        break;
                    }
                }
            }
        }
        return {server.value("id", ""), server.value("status", "BUILD"), ip};
    }

    void action(const std::string& id, const json& operation) {
        const auto response = request("POST", compute_url_ + "/servers/" + id + "/action", token(), operation.dump());
        require_success(response, "Server action", {202, 204});
    }
};
} // namespace

std::unique_ptr<ports::ComputeProvider> make_compute_provider(const std::string& provider_name) {
    if (provider_name == "mock") return std::make_unique<MockProvider>();
    if (provider_name == "openstack") return std::make_unique<OpenStackProvider>();
    throw std::runtime_error("COMPUTE_PROVIDER must be either 'mock' or 'openstack'");
}
} // namespace vmaas::mvp
