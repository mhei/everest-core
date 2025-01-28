// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest

#include "charger_informationImpl.hpp"
#include <string>
#include <vector>

namespace module {
namespace charger_information {

void charger_informationImpl::init() {
}

void charger_informationImpl::ready() {
    json info(handle_get_charger_information());

    EVLOG_info << "ChargerInformation: " << info.dump();
}

types::charger_information::ChargerInformation charger_informationImpl::handle_get_charger_information() {
    std::vector<std::string> keys = {"vendor",           "model",     "serial",   "friendly_name", "manufacturer",
                                     "manufacturer_url", "model_url", "model_no", "revision",      "board_revision",
                                     "firmware_version"};
    json info = {};

    for (auto k : keys)
        if (mod->r_kvs->call_exists(k)) {
            auto v = mod->r_kvs->call_load(k);
            info[k] = std::get<std::string>(v);
        }

    // generate fallback friendly_name
    if (info.contains("vendor") and info.contains("model") and info.contains("serial") and
        not info.contains("friendly_name")) {
        info["friendly_name"] = info["vendor"].get<std::string>() + " " + info["model"].get<std::string>() + " [" +
                                info["serial"].get<std::string>() + "]";
    }

    EVLOG_debug << "ChargerInformation: " << info.dump();

    return info;
}

} // namespace charger_information
} // namespace module
