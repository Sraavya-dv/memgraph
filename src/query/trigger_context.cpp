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

#include "query/trigger.hpp"

#include <concepts>
#include "query/typed_value.hpp"

#include <functional>
#include <optional>
#include "query/context.hpp"
#include "query/cypher_query_interpreter.hpp"
#include "query/db_accessor.hpp"
#include "query/frontend/ast/ast.hpp"
#include "query/interpret/frame.hpp"
#include "query/serialization/property_value.hpp"
#include "query/trigger_fact.hpp"
#include "query/typed_value.hpp"
#include "storage/v2/property_value.hpp"
#include "utils/memory.hpp"

namespace memgraph::query {
namespace {
template <typename T>
concept WithToMap = requires(const T value, DbAccessor *dba) {
  { value.ToMap(dba) } -> std::same_as<std::map<std::string, TypedValue>>;
};

template <WithToMap T>
TypedValue ToTypedValue(const T &value, DbAccessor *dba) {
  return TypedValue{value.ToMap(dba)};
}

template <detail::ObjectAccessor TAccessor>
TypedValue ToTypedValue(const detail::CreatedObject<TAccessor> &created_object, [[maybe_unused]] DbAccessor *dba) {
  return TypedValue{created_object.object};
}

template <detail::ObjectAccessor TAccessor>
TypedValue ToTypedValue(const detail::DeletedObject<TAccessor> &deleted_object, [[maybe_unused]] DbAccessor *dba) {
  return TypedValue{deleted_object.object};
}

template <typename T>
concept WithIsValid = requires(const T value) {
  { value.IsValid() } -> std::same_as<bool>;
};

template <typename T>
concept ConvertableToTypedValue = requires(T value, DbAccessor *dba) {
  { ToTypedValue(value, dba) } -> std::same_as<TypedValue>;
}
&&WithIsValid<T>;

template <typename T>
concept LabelUpdateContext = utils::SameAsAnyOf<T, detail::SetVertexLabel, detail::RemovedVertexLabel>;

template <LabelUpdateContext TContext>
TypedValue ToTypedValue(const std::vector<TContext> &values, DbAccessor *dba) {
  std::unordered_map<storage::LabelId, std::vector<TypedValue>> vertices_by_labels;

  for (const auto &value : values) {
    if (value.IsValid()) {
      vertices_by_labels[value.label_id].emplace_back(value.object);
    }
  }

  TypedValue result{std::vector<TypedValue>{}};
  auto &typed_values = result.ValueList();
  for (auto &[label_id, vertices] : vertices_by_labels) {
    typed_values.emplace_back(std::map<std::string, TypedValue>{
        {std::string{"label"}, TypedValue(dba->LabelToName(label_id))},
        {std::string{"vertices"}, TypedValue(std::move(vertices))},
    });
  }

  return result;
}

std::string CanonicalTypedValueSummary(const memgraph::query::TypedValue &tv) {
  using TV = memgraph::query::TypedValue;

  switch (tv.type()) {
    case TV::Type::Null:
      return "null";
    case TV::Type::Bool:
      return std::string("b:") + (tv.ValueBool() ? "1" : "0");
    case TV::Type::Int:
      return "i:" + std::to_string(tv.ValueInt());
    case TV::Type::Double:
      return "d:" + std::to_string(tv.ValueDouble());
    case TV::Type::String: {
      const auto &pmr_s = tv.ValueString();  // std::pmr::string
      return "s:" + std::string(pmr_s.begin(), pmr_s.end());
    }

    case TV::Type::List:
      return "list";
    case TV::Type::Map:
      return "map";

    default:
      return "other";
  }
}

template <ConvertableToTypedValue T>
TypedValue ToTypedValue(const std::vector<T> &values, DbAccessor *dba) requires(!LabelUpdateContext<T>) {
  TypedValue result{std::vector<TypedValue>{}};
  auto &typed_values = result.ValueList();
  typed_values.reserve(values.size());

  for (const auto &value : values) {
    if (value.IsValid()) {
      typed_values.push_back(ToTypedValue(value, dba));
    }
  }

  return result;
}

template <typename T>
const char *TypeToString() {
  if constexpr (std::same_as<T, detail::CreatedObject<VertexAccessor>>) {
    return "created_vertex";
  } else if constexpr (std::same_as<T, detail::CreatedObject<EdgeAccessor>>) {
    return "created_edge";
  } else if constexpr (std::same_as<T, detail::DeletedObject<VertexAccessor>>) {
    return "deleted_vertex";
  } else if constexpr (std::same_as<T, detail::DeletedObject<EdgeAccessor>>) {
    return "deleted_edge";
  } else if constexpr (std::same_as<T, detail::SetObjectProperty<VertexAccessor>>) {
    return "set_vertex_property";
  } else if constexpr (std::same_as<T, detail::SetObjectProperty<EdgeAccessor>>) {
    return "set_edge_property";
  } else if constexpr (std::same_as<T, detail::RemovedObjectProperty<VertexAccessor>>) {
    return "removed_vertex_property";
  } else if constexpr (std::same_as<T, detail::RemovedObjectProperty<EdgeAccessor>>) {
    return "removed_edge_property";
  } else if constexpr (std::same_as<T, detail::SetVertexLabel>) {
    return "set_vertex_label";
  } else if constexpr (std::same_as<T, detail::RemovedVertexLabel>) {
    return "removed_vertex_label";
  }
}

template <typename T>
concept ContextInfo = WithToMap<T> && WithIsValid<T>;

template <ContextInfo... Args>
TypedValue Concatenate(DbAccessor *dba, const std::vector<Args> &...args) {
  const auto size = (args.size() + ...);
  TypedValue result{std::vector<TypedValue>{}};
  auto &concatenated = result.ValueList();
  concatenated.reserve(size);

  const auto add_to_concatenated = [&]<ContextInfo T>(const std::vector<T> &values) {
    for (const auto &value : values) {
      if (value.IsValid()) {
        auto map = value.ToMap(dba);
        map["event_type"] = TypeToString<T>();
        concatenated.emplace_back(std::move(map));
      }
    }
  };

  (add_to_concatenated(args), ...);

  return result;
}

template <typename T>
concept WithEmpty = requires(const T value) {
  { value.empty() } -> std::same_as<bool>;
};

template <WithEmpty... TContainer>
bool AnyContainsValue(const TContainer &...value_containers) {
  return (!value_containers.empty() || ...);
}

template <detail::ObjectAccessor TAccessor>
using ChangesSummary =
    std::tuple<std::vector<detail::CreatedObject<TAccessor>>, std::vector<detail::DeletedObject<TAccessor>>,
               std::vector<detail::SetObjectProperty<TAccessor>>,
               std::vector<detail::RemovedObjectProperty<TAccessor>>>;

template <detail::ObjectAccessor TAccessor>
using PropertyChangesLists =
    std::pair<std::vector<detail::SetObjectProperty<TAccessor>>, std::vector<detail::RemovedObjectProperty<TAccessor>>>;

template <detail::ObjectAccessor TAccessor>
[[nodiscard]] PropertyChangesLists<TAccessor> PropertyMapToList(
    query::TriggerContextCollector::PropertyChangesMap<TAccessor> &&map) {
  std::vector<detail::SetObjectProperty<TAccessor>> set_object_properties;
  std::vector<detail::RemovedObjectProperty<TAccessor>> removed_object_properties;

  for (auto it = map.begin(); it != map.end(); it = map.erase(it)) {
    const auto &[key, property_change_info] = *it;
    if (property_change_info.old_value.IsNull() && property_change_info.new_value.IsNull()) {
      // no change happened on the transaction level
      continue;
    }

    if (const auto is_equal = property_change_info.old_value == property_change_info.new_value;
        is_equal.IsBool() && is_equal.ValueBool()) {
      // no change happened on the transaction level
      continue;
    }

    if (property_change_info.new_value.IsNull()) {
      removed_object_properties.emplace_back(key.first, key.second /* property_id */,
                                             std::move(property_change_info.old_value));
    } else {
      set_object_properties.emplace_back(key.first, key.second, std::move(property_change_info.old_value),
                                         std::move(property_change_info.new_value));
    }
  }

  return PropertyChangesLists<TAccessor>{std::move(set_object_properties), std::move(removed_object_properties)};
}

template <detail::ObjectAccessor TAccessor>
[[nodiscard]] ChangesSummary<TAccessor> Summarize(query::TriggerContextCollector::Registry<TAccessor> &&registry) {
  auto [set_object_properties, removed_object_properties] = PropertyMapToList(std::move(registry.property_changes));
  std::vector<detail::CreatedObject<TAccessor>> created_objects_vec;
  created_objects_vec.reserve(registry.created_objects.size());
  std::transform(registry.created_objects.begin(), registry.created_objects.end(),
                 std::back_inserter(created_objects_vec), [](const auto &gid_and_created_object) {
                   std::cout << "Registered vertex GID: " << gid_and_created_object.first.AsUint() << std::endl;
                   return gid_and_created_object.second;
                 });

  // std::cout << " Summarize(): created_objects_vec.size() = " << created_objects_vec.size() << std::endl;

  registry.created_objects.clear();

  return {std::move(created_objects_vec), std::move(registry.deleted_objects), std::move(set_object_properties),
          std::move(removed_object_properties)};
}
}  // namespace

namespace detail {
bool SetVertexLabel::IsValid() const { return object.IsVisible(storage::View::OLD); }

std::map<std::string, TypedValue> SetVertexLabel::ToMap(DbAccessor *dba) const {
  return {{"vertex", TypedValue{object}}, {"label", TypedValue{dba->LabelToName(label_id)}}};
}

bool RemovedVertexLabel::IsValid() const { return object.IsVisible(storage::View::OLD); }

std::map<std::string, TypedValue> RemovedVertexLabel::ToMap(DbAccessor *dba) const {
  return {{"vertex", TypedValue{object}}, {"label", TypedValue{dba->LabelToName(label_id)}}};
}
auto ObjectCommonMethods::PropertyToName(DbAccessor *dba, storage::PropertyId key) -> TypedValue {
  return TypedValue{dba->PropertyToName(key)};
}
TypedValue ObjectCommonMethods::LabelToName(DbAccessor *dba, storage::LabelId lid) {
  return TypedValue{dba->LabelToName(lid)};
}

TypedValue ObjectCommonMethods::EdgeTypeToName(DbAccessor *dba, storage::EdgeTypeId et) {
  return TypedValue{dba->EdgeTypeToName(et)};
}
}  // namespace detail

TriggerContext TriggerContext::Normalize() const {
  TriggerContext out = *this;

  std::unordered_set<storage::Gid> deleted_vertices;
  deleted_vertices.reserve(out.deleted_vertices_.size());
  for (const auto &dv : out.deleted_vertices_) deleted_vertices.insert(dv.object.Gid());

  std::unordered_set<storage::Gid> deleted_edges;
  deleted_edges.reserve(out.deleted_edges_.size());
  for (const auto &de : out.deleted_edges_) deleted_edges.insert(de.object.Gid());

  // Cancel create+delete for vertices
  if (!deleted_vertices.empty()) {
    auto &cv = out.created_vertices_;
    cv.erase(
        std::remove_if(cv.begin(), cv.end(), [&](const auto &x) { return deleted_vertices.contains(x.object.Gid()); }),
        cv.end());

    // Drop all vertex updates for deleted vertices
    out.set_vertex_properties_.erase(
        std::remove_if(out.set_vertex_properties_.begin(), out.set_vertex_properties_.end(),
                       [&](const auto &x) { return deleted_vertices.contains(x.object.Gid()); }),
        out.set_vertex_properties_.end());

    out.removed_vertex_properties_.erase(
        std::remove_if(out.removed_vertex_properties_.begin(), out.removed_vertex_properties_.end(),
                       [&](const auto &x) { return deleted_vertices.contains(x.object.Gid()); }),
        out.removed_vertex_properties_.end());

    out.set_vertex_labels_.erase(
        std::remove_if(out.set_vertex_labels_.begin(), out.set_vertex_labels_.end(),
                       [&](const auto &x) { return deleted_vertices.contains(x.object.Gid()); }),
        out.set_vertex_labels_.end());

    out.removed_vertex_labels_.erase(
        std::remove_if(out.removed_vertex_labels_.begin(), out.removed_vertex_labels_.end(),
                       [&](const auto &x) { return deleted_vertices.contains(x.object.Gid()); }),
        out.removed_vertex_labels_.end());
  }

  // Cancel create+delete for edges + drop updates
  if (!deleted_edges.empty()) {
    auto &ce = out.created_edges_;
    ce.erase(
        std::remove_if(ce.begin(), ce.end(), [&](const auto &x) { return deleted_edges.contains(x.object.Gid()); }),
        ce.end());

    out.set_edge_properties_.erase(
        std::remove_if(out.set_edge_properties_.begin(), out.set_edge_properties_.end(),
                       [&](const auto &x) { return deleted_edges.contains(x.object.Gid()); }),
        out.set_edge_properties_.end());

    out.removed_edge_properties_.erase(
        std::remove_if(out.removed_edge_properties_.begin(), out.removed_edge_properties_.end(),
                       [&](const auto &x) { return deleted_edges.contains(x.object.Gid()); }),
        out.removed_edge_properties_.end());
  }

  return out;
}

const char *TriggerEventTypeToString(const TriggerEventType event_type) {
  switch (event_type) {
    case TriggerEventType::ANY:
      return "ANY";

    case TriggerEventType::CREATE:
      return "CREATE";

    case TriggerEventType::VERTEX_CREATE:
      return "() CREATE";

    case TriggerEventType::EDGE_CREATE:
      return "--> CREATE";

    case TriggerEventType::DELETE:
      return "DELETE";

    case TriggerEventType::VERTEX_DELETE:
      return "() DELETE";

    case TriggerEventType::EDGE_DELETE:
      return "--> DELETE";

    case TriggerEventType::UPDATE:
      return "UPDATE";

    case TriggerEventType::VERTEX_UPDATE:
      return "() UPDATE";

    case TriggerEventType::EDGE_UPDATE:
      return "--> UPDATE";
  }
}

void TriggerContext::AdaptForAccessor(DbAccessor *accessor) {
  {
    // adapt created_vertices_
    auto it = created_vertices_.begin();
    for (auto &created_vertex : created_vertices_) {
      if (auto maybe_vertex = accessor->FindVertex(created_vertex.object.Gid(), storage::View::OLD); maybe_vertex) {
        *it = detail::CreatedObject{*maybe_vertex};
        ++it;
      }
    }
    created_vertices_.erase(it, created_vertices_.end());
  }

  const auto adapt_context_with_vertex = [accessor](auto *values) {
    auto it = values->begin();
    for (auto &value : *values) {
      if (auto maybe_vertex = accessor->FindVertex(value.object.Gid(), storage::View::OLD); maybe_vertex) {
        *it = std::move(value);
        it->object = *maybe_vertex;
        ++it;
      }
    }
    values->erase(it, values->end());
  };

  adapt_context_with_vertex(&set_vertex_properties_);
  adapt_context_with_vertex(&removed_vertex_properties_);
  adapt_context_with_vertex(&set_vertex_labels_);
  adapt_context_with_vertex(&removed_vertex_labels_);

  {
    // adapt created_edges
    auto it = created_edges_.begin();
    for (auto &created_edge : created_edges_) {
      const auto maybe_from_vertex = accessor->FindVertex(created_edge.object.From().Gid(), storage::View::OLD);
      if (!maybe_from_vertex) {
        continue;
      }
      auto maybe_out_edges = maybe_from_vertex->OutEdges(storage::View::OLD);
      MG_ASSERT(maybe_out_edges.HasValue());
      const auto edge_gid = created_edge.object.Gid();
      for (const auto &edge : maybe_out_edges->edges) {
        if (edge.Gid() == edge_gid) {
          *it = detail::CreatedObject{edge};
          ++it;
        }
      }
    }
    created_edges_.erase(it, created_edges_.end());
  }

  // deleted_edges_ should keep the transaction context of the transaction which deleted it
  // because no other transaction can modify an object after it's deleted so it should be the
  // latest state of the object

  const auto adapt_context_with_edge = [accessor](auto *values) {
    auto it = values->begin();
    for (const auto &value : *values) {
      if (auto maybe_vertex = accessor->FindVertex(value.object.From().Gid(), storage::View::OLD); maybe_vertex) {
        auto maybe_out_edges = maybe_vertex->OutEdges(storage::View::OLD);
        MG_ASSERT(maybe_out_edges.HasValue());
        for (const auto &edge : maybe_out_edges->edges) {
          if (edge.Gid() == value.object.Gid()) {
            *it = std::move(value);
            it->object = edge;
            ++it;
            break;
          }
        }
      }
    }
    values->erase(it, values->end());
  };

  adapt_context_with_edge(&set_edge_properties_);
  adapt_context_with_edge(&removed_edge_properties_);
}

TypedValue TriggerContext::GetTypedValue(const TriggerIdentifierTag tag, DbAccessor *dba) const {
  switch (tag) {
    case TriggerIdentifierTag::CREATED_VERTICES:
      return ToTypedValue(created_vertices_, dba);

    case TriggerIdentifierTag::CREATED_EDGES:
      return ToTypedValue(created_edges_, dba);

    case TriggerIdentifierTag::CREATED_OBJECTS:
      return Concatenate(dba, created_vertices_, created_edges_);

    case TriggerIdentifierTag::DELETED_VERTICES:
      return ToTypedValue(deleted_vertices_, dba);

    case TriggerIdentifierTag::DELETED_EDGES:
      return ToTypedValue(deleted_edges_, dba);

    case TriggerIdentifierTag::DELETED_OBJECTS:
      return Concatenate(dba, deleted_vertices_, deleted_edges_);

    case TriggerIdentifierTag::SET_VERTEX_PROPERTIES:
      return ToTypedValue(set_vertex_properties_, dba);

    case TriggerIdentifierTag::SET_EDGE_PROPERTIES:
      return ToTypedValue(set_edge_properties_, dba);

    case TriggerIdentifierTag::REMOVED_VERTEX_PROPERTIES:
      return ToTypedValue(removed_vertex_properties_, dba);

    case TriggerIdentifierTag::REMOVED_EDGE_PROPERTIES:
      return ToTypedValue(removed_edge_properties_, dba);

    case TriggerIdentifierTag::SET_VERTEX_LABELS:
      return ToTypedValue(set_vertex_labels_, dba);

    case TriggerIdentifierTag::REMOVED_VERTEX_LABELS:
      return ToTypedValue(removed_vertex_labels_, dba);

    case TriggerIdentifierTag::UPDATED_VERTICES:
      return Concatenate(dba, set_vertex_properties_, removed_vertex_properties_, set_vertex_labels_,
                         removed_vertex_labels_);

    case TriggerIdentifierTag::UPDATED_EDGES:
      return Concatenate(dba, set_edge_properties_, removed_edge_properties_);

    case TriggerIdentifierTag::UPDATED_OBJECTS:
      return Concatenate(dba, set_vertex_properties_, set_edge_properties_, removed_vertex_properties_,
                         removed_edge_properties_, set_vertex_labels_, removed_vertex_labels_);
  }
}

bool TriggerContext::ShouldEventTrigger(const TriggerEventType event_type) const {
  using EventType = TriggerEventType;
  switch (event_type) {
    case EventType::ANY:
      return AnyContainsValue(created_vertices_, created_edges_, deleted_vertices_, deleted_edges_,
                              set_vertex_properties_, set_edge_properties_, removed_vertex_properties_,
                              removed_edge_properties_, set_vertex_labels_, removed_vertex_labels_);

    case EventType::CREATE:
      return AnyContainsValue(created_vertices_, created_edges_);

    case EventType::VERTEX_CREATE:
      return AnyContainsValue(created_vertices_);

    case EventType::EDGE_CREATE:
      return AnyContainsValue(created_edges_);

    case EventType::DELETE:
      return AnyContainsValue(deleted_vertices_, deleted_edges_);

    case EventType::VERTEX_DELETE:
      return AnyContainsValue(deleted_vertices_);

    case EventType::EDGE_DELETE:
      return AnyContainsValue(deleted_edges_);

    case EventType::UPDATE:
      return AnyContainsValue(set_vertex_properties_, set_edge_properties_, removed_vertex_properties_,
                              removed_edge_properties_, set_vertex_labels_, removed_vertex_labels_);

    case EventType::VERTEX_UPDATE:
      return AnyContainsValue(set_vertex_properties_, removed_vertex_properties_, set_vertex_labels_,
                              removed_vertex_labels_);

    case EventType::EDGE_UPDATE:
      return AnyContainsValue(set_edge_properties_, removed_edge_properties_);
  }
}

void TriggerContextCollector::UpdateLabelMap(const VertexAccessor vertex, const storage::LabelId label_id,
                                             const LabelChange change) {
  auto &registry = GetRegistry<VertexAccessor>();

  if (registry.should_register_updated_objects) {
    std::cout << "[DEBUG] Registered SET property on GID = " << vertex.Gid().AsUint() << std::endl;
  }
  if (!registry.should_register_updated_objects || registry.created_objects.count(vertex.Gid())) {
    return;
  }

  if (auto it = label_changes_.find({vertex, label_id}); it != label_changes_.end()) {
    it->second = std::clamp(it->second + LabelChangeToInt(change), -1, 1);
    return;
  }

  label_changes_.emplace(std::make_pair(vertex, label_id), LabelChangeToInt(change));
}

TriggerContextCollector::TriggerContextCollector(const std::unordered_set<TriggerEventType> &event_types) {
  for (const auto event_type : event_types) {
    switch (event_type) {
      case TriggerEventType::ANY:
        vertex_registry_.should_register_created_objects = true;
        edge_registry_.should_register_created_objects = true;
        vertex_registry_.should_register_deleted_objects = true;
        edge_registry_.should_register_deleted_objects = true;
        vertex_registry_.should_register_updated_objects = true;
        edge_registry_.should_register_updated_objects = true;
        break;
      case TriggerEventType::VERTEX_CREATE:
        vertex_registry_.should_register_created_objects = true;
        break;
      case TriggerEventType::EDGE_CREATE:
        edge_registry_.should_register_created_objects = true;
        break;
      case TriggerEventType::CREATE:
        vertex_registry_.should_register_created_objects = true;
        edge_registry_.should_register_created_objects = true;
        break;
      case TriggerEventType::VERTEX_DELETE:
        vertex_registry_.should_register_deleted_objects = true;
        break;
      case TriggerEventType::EDGE_DELETE:
        edge_registry_.should_register_deleted_objects = true;
        break;
      case TriggerEventType::DELETE:
        vertex_registry_.should_register_deleted_objects = true;
        edge_registry_.should_register_deleted_objects = true;
        break;
      case TriggerEventType::VERTEX_UPDATE:
        vertex_registry_.should_register_updated_objects = true;
        break;
      case TriggerEventType::EDGE_UPDATE:
        edge_registry_.should_register_updated_objects = true;
        break;
      case TriggerEventType::UPDATE:
        vertex_registry_.should_register_updated_objects = true;
        edge_registry_.should_register_updated_objects = true;
        break;
    }
  }

  const auto deduce_if_should_register_created = [](auto &registry) {
    // Registering the created objects is necessary to:
    // - eliminate deleted objects that were created in the same transaction
    // - eliminate set/removed properties and labels of newly created objects
    // because those changes are only relevant for objects that have existed before the transaction.
    registry.should_register_created_objects |=
        registry.should_register_updated_objects || registry.should_register_deleted_objects;
  };

  deduce_if_should_register_created(vertex_registry_);
  deduce_if_should_register_created(edge_registry_);
}

bool TriggerContextCollector::ShouldRegisterVertexLabelChange() const {
  //  return vertex_registry_.should_register_updated_objects;
  return should_register_vertex_label_changes;
}

void TriggerContextCollector::RegisterSetVertexLabel(const VertexAccessor &vertex, const storage::LabelId label_id) {
  UpdateLabelMap(vertex, label_id, LabelChange::ADD);
}

void TriggerContextCollector::RegisterRemovedVertexLabel(const VertexAccessor &vertex,
                                                         const storage::LabelId label_id) {
  UpdateLabelMap(vertex, label_id, LabelChange::REMOVE);
}

void TriggerContext::Merge(const TriggerContext &other) {
  created_vertices_.insert(created_vertices_.end(), other.created_vertices_.begin(), other.created_vertices_.end());
  deleted_vertices_.insert(deleted_vertices_.end(), other.deleted_vertices_.begin(), other.deleted_vertices_.end());
  created_edges_.insert(created_edges_.end(), other.created_edges_.begin(), other.created_edges_.end());
  deleted_edges_.insert(deleted_edges_.end(), other.deleted_edges_.begin(), other.deleted_edges_.end());
  // UPDATE events (property changes)
  set_vertex_properties_.insert(set_vertex_properties_.end(), other.set_vertex_properties_.begin(),
                                other.set_vertex_properties_.end());
  removed_vertex_properties_.insert(removed_vertex_properties_.end(), other.removed_vertex_properties_.begin(),
                                    other.removed_vertex_properties_.end());
  set_edge_properties_.insert(set_edge_properties_.end(), other.set_edge_properties_.begin(),
                              other.set_edge_properties_.end());
  removed_edge_properties_.insert(removed_edge_properties_.end(), other.removed_edge_properties_.begin(),
                                  other.removed_edge_properties_.end());

  // UPDATE events (label changes)
  set_vertex_labels_.insert(set_vertex_labels_.end(), other.set_vertex_labels_.begin(), other.set_vertex_labels_.end());
  removed_vertex_labels_.insert(removed_vertex_labels_.end(), other.removed_vertex_labels_.begin(),
                                other.removed_vertex_labels_.end());
}

int8_t TriggerContextCollector::LabelChangeToInt(LabelChange change) {
  static_assert(std::is_same_v<std::underlying_type_t<LabelChange>, int8_t>,
                "The underlying type of LabelChange doesn't match the return type!");
  return static_cast<int8_t>(change);
}

TriggerContext TriggerContextCollector::TransformToTriggerContext() && {
  auto [created_vertices, deleted_vertices, set_vertex_properties, removed_vertex_properties] =
      Summarize(std::move(vertex_registry_));
  auto [set_vertex_labels, removed_vertex_labels] = LabelMapToList(std::move(label_changes_));
  auto [created_edges, deleted_edges, set_edge_properties, removed_edge_properties] =
      Summarize(std::move(edge_registry_));

  deleted_edges.reserve(deleted_edges.size() + deleted_edges_buffer_.size());
  for (auto &de : deleted_edges_buffer_) {
    deleted_edges.push_back(std::move(de));
  }

  set_edge_properties.reserve(set_edge_properties.size() + updated_edge_properties_buffer_.size());
  for (auto &up : updated_edge_properties_buffer_) {
    set_edge_properties.push_back(std::move(up));
  }

  removed_edge_properties.reserve(removed_edge_properties.size() + removed_edge_properties_buffer_.size());
  for (auto &rp : removed_edge_properties_buffer_) {
    removed_edge_properties.push_back(std::move(rp));
  }

  // Append “positive‐diff” edge buffer into created_edges
  created_edges.reserve(created_edges.size() + inserted_edges_.size());
  for (auto &ce : inserted_edges_) {
    created_edges.push_back(std::move(ce));
  }

  // Now construct the TriggerContext with the augmented created_edges
  return {std::move(created_vertices),      std::move(deleted_vertices),
          std::move(set_vertex_properties), std::move(removed_vertex_properties),
          std::move(set_vertex_labels),     std::move(removed_vertex_labels),
          std::move(created_edges),         std::move(deleted_edges),
          std::move(set_edge_properties),   std::move(removed_edge_properties)};
}

TriggerContext TriggerContext::FilterByEventType(TriggerEventType type) const {
  TriggerContext filtered;

  auto is_create = (type == TriggerEventType::ANY || type == TriggerEventType::CREATE ||
                    type == TriggerEventType::VERTEX_CREATE || type == TriggerEventType::EDGE_CREATE);
  auto is_delete = (type == TriggerEventType::ANY || type == TriggerEventType::DELETE ||
                    type == TriggerEventType::VERTEX_DELETE || type == TriggerEventType::EDGE_DELETE);
  auto is_update = (type == TriggerEventType::ANY || type == TriggerEventType::UPDATE ||
                    type == TriggerEventType::VERTEX_UPDATE || type == TriggerEventType::EDGE_UPDATE);

  if (is_create) {
    filtered.created_vertices_ = created_vertices_;
    filtered.created_edges_ = created_edges_;
  }

  if (is_delete) {
    filtered.deleted_vertices_ = deleted_vertices_;
    filtered.deleted_edges_ = deleted_edges_;
  }

  if (is_update) {
    // PROP
    filtered.set_vertex_properties_ = set_vertex_properties_;
    filtered.removed_vertex_properties_ = removed_vertex_properties_;
    filtered.set_edge_properties_ = set_edge_properties_;
    filtered.removed_edge_properties_ = removed_edge_properties_;
    // LABELS
    filtered.set_vertex_labels_ = set_vertex_labels_;
    filtered.removed_vertex_labels_ = removed_vertex_labels_;
  }

  // Narrow further for vertex-only or edge-only update types
  if (type == TriggerEventType::VERTEX_UPDATE) {
    filtered.set_edge_properties_.clear();
    filtered.removed_edge_properties_.clear();
  }
  if (type == TriggerEventType::EDGE_UPDATE) {
    filtered.set_vertex_properties_.clear();
    filtered.removed_vertex_properties_.clear();
    filtered.set_vertex_labels_.clear();
    filtered.removed_vertex_labels_.clear();
  }

  // Narrow further for vertex-only create/delete
  if (type == TriggerEventType::VERTEX_CREATE) filtered.created_edges_.clear();
  if (type == TriggerEventType::EDGE_CREATE) filtered.created_vertices_.clear();
  if (type == TriggerEventType::VERTEX_DELETE) filtered.deleted_edges_.clear();
  if (type == TriggerEventType::EDGE_DELETE) filtered.deleted_vertices_.clear();

  return filtered;
}

TriggerContextCollector::LabelChangesLists TriggerContextCollector::LabelMapToList(LabelChangesMap &&label_changes) {
  std::vector<detail::SetVertexLabel> set_vertex_labels;
  std::vector<detail::RemovedVertexLabel> removed_vertex_labels;

  for (const auto &[key, label_state] : label_changes) {
    if (label_state == LabelChangeToInt(LabelChange::ADD)) {
      set_vertex_labels.emplace_back(key.first, key.second);
    } else if (label_state == LabelChangeToInt(LabelChange::REMOVE)) {
      removed_vertex_labels.emplace_back(key.first, key.second);
    }
  }

  label_changes.clear();

  return {std::move(set_vertex_labels), std::move(removed_vertex_labels)};
}

TriggerFactSet TriggerContext::ExtractFactSignatures(storage::View view, DbAccessor *dba) const {
  TriggerFactSet facts;

  auto to_std = [](auto pmr_str) { return std::string{pmr_str}; };

  auto prop_name = [&](storage::PropertyId pid) -> std::string {
    return to_std(detail::ObjectCommonMethods::PropertyToName(dba, pid).ValueString());
  };

  auto label_name = [&](storage::LabelId lid) -> std::string {
    return to_std(detail::ObjectCommonMethods::LabelToName(dba, lid).ValueString());
  };

  auto edge_type_name = [&](storage::EdgeTypeId et) -> std::string {
    return to_std(detail::ObjectCommonMethods::EdgeTypeToName(dba, et).ValueString());
  };

  auto summarize_value = [&](const storage::PropertyValue &v) -> std::string {
    if (v.IsNull()) return "null";
    if (v.IsBool()) return v.ValueBool() ? "true" : "false";
    if (v.IsInt()) return std::to_string(v.ValueInt());
    if (v.IsDouble()) return std::to_string(v.ValueDouble());
    if (v.IsString()) return v.ValueString();
    return std::string{"<complex>"};
  };

  // VERTEX: property sets
  for (const auto &obj : set_vertex_properties_) {
    if (!obj.IsValid()) continue;
    //",const std::string name = prop_name(obj.key);
    std::string val_sum;

    /*auto props_res = obj.object.Properties(view);
    if (!props_res.HasError()) {
      const auto &props = props_res.GetValue();
      auto it = props.find(obj.key);
      if (it != props.end()) val_sum = summarize_value(it->second);
    }*/

    const std::string prop_name = dba->PropertyToName(obj.key);

    const std::string old_sum = CanonicalTypedValueSummary(obj.old_value);
    const std::string new_sum = CanonicalTypedValueSummary(obj.new_value);

    facts.insert(TriggerFactSignature{"SET_VERTEX_PROP", obj.object.Gid(), prop_name, old_sum, new_sum});
  }

  // VERTEX: property removals
  for (const auto &obj : removed_vertex_properties_) {
    if (!obj.IsValid()) continue;
    const std::string prop_name = dba->PropertyToName(obj.key);
    const std::string old_sum = CanonicalTypedValueSummary(obj.old_value);

    facts.insert(TriggerFactSignature{"REMOVED_VERTEX_PROP", obj.object.Gid(), prop_name, old_sum,
                                      std::make_optional<std::string>("null")});
  }

  // VERTEX: label add/remove  (NOTE: label_id, not label)
  for (const auto &obj : set_vertex_labels_) {
    const std::string lbl = label_name(obj.label_id);
    facts.insert(TriggerFactSignature{"ADD_VERTEX_LABEL", obj.object.Gid(), std::make_optional(lbl), std::nullopt,
                                      std::nullopt});
  }

  for (const auto &obj : removed_vertex_labels_) {
    const std::string lbl = label_name(obj.label_id);
    facts.insert(TriggerFactSignature{"REMOVE_VERTEX_LABEL", obj.object.Gid(), std::make_optional(lbl), std::nullopt,
                                      std::nullopt});
  }

  for (const auto &obj : created_edges_) {
    const auto &e = obj.object;
    const auto from = e.From().Gid();
    const auto to = e.To().Gid();
    const std::string et = edge_type_name(e.EdgeType());
    const std::string vs = "to=" + std::to_string(to.AsUint());

    facts.insert(TriggerFactSignature{"CREATED_EDGE", from, std::make_optional(et), std::make_optional(vs)});
  }

  for (const auto &obj : deleted_edges_) {
    const auto &e = obj.object;
    const auto from = e.From().Gid();
    const auto to = e.To().Gid();
    const std::string et = edge_type_name(e.EdgeType());
    const std::string vs = "to=" + std::to_string(to.AsUint());

    facts.insert(TriggerFactSignature{"DELETED_EDGE", from, std::make_optional(et), std::make_optional(vs)});
  }

  // --------------------
  // EDGE: property sets
  for (const auto &obj : set_edge_properties_) {
    const auto &e = obj.object;

    // Use EDGE GID for uniqueness (important if multiple edges share endpoints/type).
    const auto gid = e.Gid();

    const std::string et = edge_type_name(e.EdgeType());
    const std::string prop_name = dba->PropertyToName(obj.key);

    const std::string old_sum = CanonicalTypedValueSummary(obj.old_value);
    const std::string new_sum = CanonicalTypedValueSummary(obj.new_value);

    // property_name: namespace edge type + property
    // value_summary: encode transition old->new so dedup doesn't kill recursion
    facts.insert(TriggerFactSignature{"SET_EDGE_PROP", gid, std::make_optional(et + "." + prop_name),
                                      std::make_optional(old_sum), std::make_optional(new_sum)});
  }

  // EDGE: property removals
  for (const auto &obj : removed_edge_properties_) {
    const auto &e = obj.object;
    const auto gid = e.Gid();

    const std::string et = edge_type_name(e.EdgeType());
    const std::string prop_name = dba->PropertyToName(obj.key);

    const std::string old_sum = CanonicalTypedValueSummary(obj.old_value);

    facts.insert(TriggerFactSignature{"REMOVED_EDGE_PROP", gid, std::make_optional(et + "." + prop_name),
                                      std::make_optional(old_sum), "null"});
  }

  for (const auto &obj : created_vertices_) {
    facts.insert(TriggerFactSignature{"CREATED_VERTEX", obj.object.Gid(), std::nullopt, std::nullopt});
  }

  for (const auto &obj : deleted_vertices_) {
    facts.insert(TriggerFactSignature{"DELETED_VERTEX", obj.object.Gid(), std::nullopt, std::nullopt});
  }

  return facts;
}

}  // namespace memgraph::query
