// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest
#include "ChargerInfo.hpp"

namespace module {

void ChargerInfo::init() {
    invoke_init(*p_charger_information);
}

void ChargerInfo::ready() {
    invoke_ready(*p_charger_information);
}

} // namespace module
