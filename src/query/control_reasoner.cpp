// Copyright 2024 Memgraph Ltd.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.txt; by using this file, you agree to be bound by the terms of the Business Source
// License, and you may not use this file except in compliance with the Business Source License.
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0, included in the file
// licenses/APL.txt.

// Step 1: Build new ownership map
std::map<std::pair<VertexId, VertexId>, float> ownership;

for (edge : db.OWN edges) ownership[(a,b)] += edge.pct;
for (edge : context.created_edges) ownership[(a,b)] += edge.pct;
for (edge : context.deleted_edges) ownership[(a,b)] -= edge.pct;

// Step 2: Compute control recursively (DFS-style)
for each x:
  for each y:
    float total = AggregateOwnershipOverControlPaths(x, y, ownership)
    if total > 0.5: emit control⁺(x, y)
    if total <= 0.5 and previous control(x, y) exists: emit control⁻(x, y)
