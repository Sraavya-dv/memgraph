// Copyright 2026 Memgraph Ltd.
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
#include "storage/v2/storage.hpp"

namespace memgraph::query {

struct TriggerFactSignature {
  std::string type;
  storage::Gid gid;

  // For property facts
  std::optional<std::string> property_name;

  std::optional<std::string> old_value_summary;
  std::optional<std::string> new_value_summary;

  bool operator==(const TriggerFactSignature &o) const {
    return type == o.type && gid == o.gid && property_name == o.property_name &&
           old_value_summary == o.old_value_summary && new_value_summary == o.new_value_summary;
  }

  struct Hash {
    std::size_t operator()(const TriggerFactSignature &f) const {
      std::size_t h = std::hash<std::string>()(f.type);
      h = h * 31 + std::hash<uint64_t>()(f.gid.AsUint());
      if (f.property_name) h = h * 31 + std::hash<std::string>()(*f.property_name);
      if (f.old_value_summary) h = h * 31 + std::hash<std::string>()(*f.old_value_summary);
      if (f.new_value_summary) h = h * 31 + std::hash<std::string>()(*f.new_value_summary);
      return h;
    }
  };
};

using TriggerFactSet = std::unordered_set<TriggerFactSignature, TriggerFactSignature::Hash>;

}  // namespace memgraph::query
