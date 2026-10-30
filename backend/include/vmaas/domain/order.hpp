#pragma once
#include <string>
#include <string_view>

namespace vmaas::domain {
enum class OrderState { Pending, Provisioning, Ready, Failed, Deleting, Deleted, Cancelled };

inline std::string_view to_string(OrderState state) {
    switch (state) {
    case OrderState::Pending: return "PENDING";
    case OrderState::Provisioning: return "PROVISIONING";
    case OrderState::Ready: return "READY";
    case OrderState::Failed: return "FAILED";
    case OrderState::Deleting: return "DELETING";
    case OrderState::Deleted: return "DELETED";
    case OrderState::Cancelled: return "CANCELLED";
    }
    return "UNKNOWN";
}

struct Order {
    std::string id;
    std::string project_id;
    std::string requested_by;
    std::string image_id;
    std::string flavor_id;
    std::string vm_name;
    OrderState state{OrderState::Pending};
    std::string correlation_id;
};
}
