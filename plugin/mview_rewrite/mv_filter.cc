/* Copyright (c) 2026, Justin Swanhart and contributors.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/**
  @file plugin/mview_rewrite/mv_filter.cc

  The filter tree of section 4 of the paper: views are partitioned by a
  key at each level, and the keys of a level are kept in a lattice index
  so that the subsets (or supersets) of the key of a query are found
  without looking at every key.
*/

#include "plugin/mview_rewrite/mview_rewrite.h"

#include <algorithm>
#include <map>

namespace mv_rewrite {

namespace {

bool is_subset(const std::vector<int> &a, const std::vector<int> &b) {
  return std::includes(b.begin(), b.end(), a.begin(), a.end());
}

}  // namespace

void Lattice::build(std::vector<std::pair<std::vector<int>, int>> entries) {
  m_nodes.clear();
  m_roots.clear();
  m_tops.clear();

  std::map<std::vector<int>, std::vector<int>> by_key;
  for (auto &entry : entries) {
    std::sort(entry.first.begin(), entry.first.end());
    entry.first.erase(std::unique(entry.first.begin(), entry.first.end()),
                      entry.first.end());
    by_key[entry.first].push_back(entry.second);
  }
  std::vector<std::pair<std::vector<int>, std::vector<int>>> keys(
      by_key.begin(), by_key.end());
  /* smaller sets first: a key inserted later is never a subset of one
     inserted before, so only its maximal subsets need to be found */
  std::stable_sort(keys.begin(), keys.end(), [](const auto &a, const auto &b) {
    return a.first.size() < b.first.size();
  });

  std::vector<int> stamp;
  int generation = 0;
  for (auto &key : keys) {
    const int index = static_cast<int>(m_nodes.size());
    Node node;
    node.key = std::move(key.first);
    node.payloads = std::move(key.second);

    /* the subsets of the new key, from the roots upwards */
    generation++;
    stamp.resize(m_nodes.size(), 0);
    std::vector<int> subsets;
    std::vector<int> stack;
    for (int r : m_roots) {
      if (is_subset(m_nodes[r].key, node.key) && stamp[r] != generation) {
        stamp[r] = generation;
        stack.push_back(r);
      }
    }
    while (!stack.empty()) {
      const int n = stack.back();
      stack.pop_back();
      subsets.push_back(n);
      for (int up : m_nodes[n].supersets) {
        if (stamp[up] == generation) continue;
        if (!is_subset(m_nodes[up].key, node.key)) continue;
        stamp[up] = generation;
        stack.push_back(up);
      }
    }
    /* the maximal ones: no superset pointer to another subset */
    for (int n : subsets) {
      bool maximal = true;
      for (int up : m_nodes[n].supersets)
        if (is_subset(m_nodes[up].key, node.key)) maximal = false;
      if (!maximal) continue;
      node.subsets.push_back(n);
      m_nodes[n].supersets.push_back(index);
    }
    if (node.subsets.empty()) m_roots.push_back(index);
    m_nodes.push_back(std::move(node));
  }
  for (size_t i = 0; i < m_nodes.size(); i++)
    if (m_nodes[i].supersets.empty()) m_tops.push_back(static_cast<int>(i));
}

void Lattice::subsets_of(const std::vector<int> &q,
                         std::vector<int> *out) const {
  std::vector<bool> seen(m_nodes.size(), false);
  std::vector<int> stack;
  for (int r : m_roots) {
    if (!is_subset(m_nodes[r].key, q)) continue;
    seen[r] = true;
    stack.push_back(r);
  }
  while (!stack.empty()) {
    const int n = stack.back();
    stack.pop_back();
    out->insert(out->end(), m_nodes[n].payloads.begin(),
                m_nodes[n].payloads.end());
    /* a superset of a key that is not a subset of q is not either */
    for (int up : m_nodes[n].supersets) {
      if (seen[up] || !is_subset(m_nodes[up].key, q)) continue;
      seen[up] = true;
      stack.push_back(up);
    }
  }
}

void Lattice::supersets_of(const std::vector<int> &q,
                           std::vector<int> *out) const {
  std::vector<bool> seen(m_nodes.size(), false);
  std::vector<int> stack;
  for (int t : m_tops) {
    if (!is_subset(q, m_nodes[t].key)) continue;
    seen[t] = true;
    stack.push_back(t);
  }
  while (!stack.empty()) {
    const int n = stack.back();
    stack.pop_back();
    out->insert(out->end(), m_nodes[n].payloads.begin(),
                m_nodes[n].payloads.end());
    for (int down : m_nodes[n].subsets) {
      if (seen[down] || !is_subset(q, m_nodes[down].key)) continue;
      seen[down] = true;
      stack.push_back(down);
    }
  }
}

void Filter_tree::build(const std::vector<View> &views) {
  /* level 1: hubs */
  std::map<std::vector<int>, std::vector<int>> by_hub;
  for (size_t i = 0; i < views.size(); i++)
    by_hub[views[i].hub_key].push_back(static_cast<int>(i));

  std::vector<std::pair<std::vector<int>, int>> hub_entries;
  m_residual_levels.clear();
  m_leaves.clear();
  for (const auto &hub : by_hub) {
    const int level = static_cast<int>(m_residual_levels.size());
    hub_entries.emplace_back(hub.first, level);
    m_residual_levels.emplace_back();

    /* level 2: residual predicates */
    std::map<std::vector<int>, std::vector<int>> by_residual;
    for (int v : hub.second) by_residual[views[v].residual_key].push_back(v);
    std::vector<std::pair<std::vector<int>, int>> residual_entries;
    for (const auto &residual : by_residual) {
      const int leaf = static_cast<int>(m_leaves.size());
      residual_entries.emplace_back(residual.first, leaf);
      m_leaves.emplace_back();
      /* leaves: join views; aggregate views by source tables */
      std::vector<std::pair<std::vector<int>, int>> source_entries;
      for (int v : residual.second) {
        if (views[v].is_aggregate)
          source_entries.emplace_back(views[v].source_key, v);
        else
          m_leaves[leaf].join_views.push_back(v);
      }
      m_leaves[leaf].aggregate_views.build(std::move(source_entries));
    }
    m_residual_levels[level].lattice.build(std::move(residual_entries));
  }
  m_hubs.build(std::move(hub_entries));
}

void Filter_tree::search(const Query_keys &keys, bool aggregate_views,
                         std::vector<int> *views) const {
  std::vector<int> levels;
  m_hubs.subsets_of(keys.tables, &levels);
  for (int level : levels) {
    std::vector<int> leaves;
    m_residual_levels[level].lattice.subsets_of(keys.residuals, &leaves);
    for (int leaf : leaves) {
      const Leaf &l = m_leaves[leaf];
      views->insert(views->end(), l.join_views.begin(), l.join_views.end());
      if (aggregate_views) l.aggregate_views.supersets_of(keys.tables, views);
    }
  }
  std::sort(views->begin(), views->end());
  views->erase(std::unique(views->begin(), views->end()), views->end());
}

}  // namespace mv_rewrite
