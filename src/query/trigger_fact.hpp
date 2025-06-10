// Copyright 2025 Memgraph Ltd.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.txt; by using this file, you agree to be bound by the terms of the Business Source
// License, and you may not use this file except in compliance with the Business Source License.
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0, included in the file
// licenses/APL.txt.
// File: src/query/trigger_fact.hpp
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <unordered_set>

#include "storage/v2/gid.hpp"

namespace memgraph::query {

struct TriggerFactSignature {
  std::string type;
  storage::Gid gid;
  std::optional<std::string> property_name;
  std::optional<std::string> label;
  std::optional<std::string> value_summary;

  bool operator==(const TriggerFactSignature &other) const {
    return type == other.type && gid == other.gid && property_name == other.property_name && label == other.label &&
           value_summary == other.value_summary;
  }
};

}  // namespace memgraph::query

namespace std {
template <>
struct hash<memgraph::query::TriggerFactSignature> {
  std::size_t operator()(const memgraph::query::TriggerFactSignature &sig) const {
    std::size_t h1 = std::hash<std::string>{}(sig.type);
    std::size_t h2 = std::hash<uint64_t>{}(sig.gid.AsUint());
    std::size_t h3 = sig.property_name ? std::hash<std::string>{}(*sig.property_name) : 0;
    std::size_t h4 = sig.label ? std::hash<std::string>{}(*sig.label) : 0;
    std::size_t h5 = sig.value_summary ? std::hash<std::string>{}(*sig.value_summary) : 0;

    return (((h1 ^ (h2 << 1)) ^ (h3 << 2)) ^ (h4 << 3)) ^ (h5 << 4);
  }
};
}  // namespace std
