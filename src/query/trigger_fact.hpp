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
#include "storage/v2/storage.hpp"  // for storage::Gid

namespace memgraph::query {

/// A “fact” produced by a trigger, used to dedupe and detect new changes.
struct TriggerFactSignature {
  std::string                   type;
  storage::Gid                  gid;
  std::optional<std::string>    property_name;
  std::optional<std::string>    value_summary;

  TriggerFactSignature(std::string t, storage::Gid g,
                       std::optional<std::string> name,
                       std::optional<std::string> val)
      : type(std::move(t)), gid(g),
        property_name(std::move(name)),
        value_summary(std::move(val)) {}

  bool operator==(const TriggerFactSignature &o) const {
    return type == o.type && gid == o.gid &&
           property_name == o.property_name &&
           value_summary == o.value_summary;
  }

  struct Hash {
    std::size_t operator()(const TriggerFactSignature &f) const {
      // 1) hash the type
      std::size_t h = std::hash<std::string>()(f.type);
      // 2) mix in the gid
      h = h * 31 + std::hash<uint64_t>()(f.gid.AsUint());
      // 3) mix in the property name if any
      if (f.property_name) {
        h = h * 31 + std::hash<std::string>()(*f.property_name);
      }
      // 4) mix in the new-value summary if any
      if (f.value_summary) {
        h = h * 31 + std::hash<std::string>()(*f.value_summary);
      }
      return h;
    }
  };
};

}  // namespace memgraph::query
