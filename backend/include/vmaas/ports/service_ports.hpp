#pragma once
#include <string>
#include "vmaas/domain/order.hpp"

namespace vmaas::ports {
struct CreateVmRequest {
    std::string order_id;
    std::string name;
    std::string image_provider_id;
    std::string flavor_name;
};
struct VmInfo { std::string provider_id, state, private_ip; };

class ComputeProvider {
public:
    virtual ~ComputeProvider() = default;
    virtual VmInfo create(const CreateVmRequest&) = 0;
    virtual VmInfo inspect(const std::string& provider_id) = 0;
    virtual void start(const std::string& provider_id) = 0;
    virtual void stop(const std::string& provider_id) = 0;
    virtual void reboot(const std::string& provider_id) = 0;
    virtual void remove(const std::string& provider_id) = 0;
};

class OrderRepository {
public:
    virtual ~OrderRepository() = default;
    virtual void save(const domain::Order&) = 0;
    virtual bool exists_by_idempotency_key(const std::string& project_id,
                                           const std::string& user_id,
                                           const std::string& key) = 0;
};
}
