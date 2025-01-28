// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest
#include "YamlStore.hpp"

namespace module {

void YamlStore::init() {
    invoke_init(*p_kvs);
}

void YamlStore::ready() {
    invoke_ready(*p_kvs);
}

} // namespace module
