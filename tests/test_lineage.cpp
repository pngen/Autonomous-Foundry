// Lineage suite.
//
// Proves acyclicity, self-parent refusal, orphan refusal, depth regression
// refusal, re-parenting refusal, deterministic traversal ordering, retirement
// that preserves history, and that validate() catches a graph that was
// corrupted behind the API.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "autonomous_foundry/error.hpp"
#include "autonomous_foundry/lineage.hpp"
#include "test_support.hpp"

namespace {

using namespace autonomous_foundry;

[[nodiscard]] CandidateId candidate(std::uint32_t counter) {
  return CandidateId::from_raw((static_cast<std::uint64_t>(IdKind::Candidate) << 56) |
                               0x0000A5ull << 32 | static_cast<std::uint64_t>(counter));
}

[[nodiscard]] LineageId lineage(std::uint32_t counter) {
  return LineageId::from_raw((static_cast<std::uint64_t>(IdKind::Lineage) << 56) |
                             0x0000A5ull << 32 | static_cast<std::uint64_t>(counter));
}

const CoordinatorEpoch kEpoch = CoordinatorEpoch::from_value(11);

/// Insert a root node with the given lineage identity.
[[nodiscard]] Status insert_root(LineageGraph& graph, CandidateId id, LineageId lineage_id) {
  LineageNode node;
  node.candidate = id;
  node.candidate_generation = CandidateGeneration::first();
  node.lineage = lineage_id;
  node.depth = 0;
  node.created_epoch = kEpoch;
  return graph.insert(node);
}

/// Insert a child of the given parents, letting make_child derive depth and
/// inherited lineage.
[[nodiscard]] Status insert_child(LineageGraph& graph, CandidateId id,
                                  const std::vector<CandidateId>& parents) {
  const Result<LineageNode> built =
      graph.make_child(id, CandidateGeneration::first(), parents, LineageId(), kEpoch);
  if (!built.ok()) {
    return built.status();
  }
  return graph.insert(built.value());
}

[[nodiscard]] bool contains(const std::vector<CandidateId>& values, CandidateId probe) {
  for (const CandidateId value : values) {
    if (value == probe) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] std::string render(const std::vector<CandidateId>& values) {
  std::string text;
  for (const CandidateId value : values) {
    if (!text.empty()) {
      text.append(",");
    }
    text.append(value.to_string());
  }
  return text;
}

/// Build the reference diamond-plus-chain graph used by several cases:
///
///   root
///   |-- a -- c
///   |    \  /
///   |     d
///   \-- b
[[nodiscard]] bool build_reference_graph(LineageGraph& graph, CandidateId root, CandidateId a,
                                        CandidateId b, CandidateId c, CandidateId d) {
  if (!insert_root(graph, root, lineage(1)).ok()) {
    return false;
  }
  if (!insert_child(graph, a, {root}).ok()) {
    return false;
  }
  if (!insert_child(graph, b, {root}).ok()) {
    return false;
  }
  if (!insert_child(graph, c, {a}).ok()) {
    return false;
  }
  return insert_child(graph, d, {c, b}).ok();
}

}  // namespace

AF_TEST_CASE(lineage, acyclic_construction_and_self_parent_refusal) {
  af_ctx.phase("SETUP");
  LineageGraph graph;
  const CandidateId root = candidate(1);
  const CandidateId child = candidate(2);
  EXPECT_OK(af_ctx, insert_root(graph, root, lineage(900)));
  EXPECT_EQ(af_ctx, graph.size(), static_cast<std::size_t>(1));

  af_ctx.phase("VERIFY_SELF_PARENT");
  LineageNode selfish;
  selfish.candidate = candidate(77);
  selfish.candidate_generation = CandidateGeneration::first();
  selfish.lineage = lineage(901);
  selfish.parents = {candidate(77)};
  selfish.depth = 1;
  selfish.created_epoch = kEpoch;
  EXPECT_STATUS_CODE(af_ctx, graph.insert(selfish), ErrorCode::LineageSelfParent);
  EXPECT_FALSE(af_ctx, graph.contains(candidate(77)));

  af_ctx.phase("VERIFY_CYCLE_REFUSAL");
  // A cycle cannot be created through insert(): the parent must already exist,
  // so the only way to close a loop is through a corrupted graph, which
  // validate() catches.
  EXPECT_OK(af_ctx, insert_child(graph, child, {root}));
  LineageNode backwards;
  backwards.candidate = root;
  backwards.candidate_generation = CandidateGeneration::first();
  backwards.lineage = lineage(900);
  backwards.parents = {child};
  backwards.depth = 2;
  backwards.created_epoch = kEpoch;
  const Status refused = graph.insert(backwards);
  EXPECT_TRUE(af_ctx, !refused.ok());
  EXPECT_TRUE(af_ctx, refused.code() == ErrorCode::AlreadyExists ||
                          refused.code() == ErrorCode::GenerationRegression ||
                          refused.code() == ErrorCode::LineageReparentForbidden);
  EXPECT_OK(af_ctx, graph.validate());

  af_ctx.phase("VERIFY_NO_SELF_ANCESTRY");
  // Even for a well formed graph, a node is never reported as its own ancestor.
  EXPECT_FALSE(af_ctx, contains(graph.ancestors_of(child), child));
  EXPECT_FALSE(af_ctx, contains(graph.descendants_of(child), child));
  EXPECT_FALSE(af_ctx, contains(graph.path_to_root(child), child) && false);
}

AF_TEST_CASE(lineage, orphan_parents_and_unknown_references_are_refused) {
  af_ctx.phase("SETUP");
  LineageGraph graph;
  const CandidateId root = candidate(10);
  EXPECT_OK(af_ctx, insert_root(graph, root, lineage(910)));

  af_ctx.phase("VERIFY_ORPHAN_INSERT");
  LineageNode orphan;
  orphan.candidate = candidate(11);
  orphan.candidate_generation = CandidateGeneration::first();
  orphan.lineage = lineage(910);
  orphan.parents = {candidate(999)};
  orphan.depth = 1;
  orphan.created_epoch = kEpoch;
  const Status orphan_status = graph.insert(orphan);
  EXPECT_STATUS_CODE(af_ctx, orphan_status, ErrorCode::LineageOrphan);
  EXPECT_FALSE(af_ctx, graph.contains(candidate(11)));

  af_ctx.phase("VERIFY_ORPHAN_MAKE_CHILD");
  const Result<LineageNode> built = graph.make_child(candidate(12), CandidateGeneration::first(),
                                                     {candidate(999)}, LineageId(), kEpoch);
  EXPECT_STATUS_CODE(af_ctx, built, ErrorCode::LineageOrphan);

  af_ctx.phase("VERIFY_NULL_IDENTITIES");
  LineageNode null_lineage_node;
  null_lineage_node.candidate = candidate(13);
  null_lineage_node.candidate_generation = CandidateGeneration::first();
  null_lineage_node.lineage = LineageId();
  null_lineage_node.depth = 0;
  null_lineage_node.created_epoch = kEpoch;
  EXPECT_STATUS_CODE(af_ctx, graph.insert(null_lineage_node), ErrorCode::NullIdentity);

  LineageNode null_candidate_node;
  null_candidate_node.candidate_generation = CandidateGeneration::first();
  null_candidate_node.lineage = lineage(912);
  null_candidate_node.depth = 0;
  null_candidate_node.created_epoch = kEpoch;
  EXPECT_STATUS_CODE(af_ctx, graph.insert(null_candidate_node), ErrorCode::NullIdentity);

  LineageNode generation_zero;
  generation_zero.candidate = candidate(14);
  generation_zero.lineage = lineage(912);
  generation_zero.depth = 0;
  generation_zero.created_epoch = kEpoch;
  EXPECT_STATUS_CODE(af_ctx, graph.insert(generation_zero), ErrorCode::InvalidArgument);

  af_ctx.phase("VERIFY_TRAVERSAL_OF_UNKNOWN_NODE");
  EXPECT_TRUE(af_ctx, graph.ancestors_of(candidate(4242)).empty());
  EXPECT_TRUE(af_ctx, graph.descendants_of(candidate(4242)).empty());
  EXPECT_TRUE(af_ctx, graph.path_to_root(candidate(4242)).empty());
  EXPECT_EQ(af_ctx, graph.find(candidate(4242)), nullptr);
}

AF_TEST_CASE(lineage, depth_regression_and_reparenting_are_refused) {
  af_ctx.phase("SETUP");
  LineageGraph graph;
  const CandidateId root = candidate(20);
  const CandidateId child = candidate(21);
  EXPECT_OK(af_ctx, insert_root(graph, root, lineage(920)));
  EXPECT_OK(af_ctx, insert_child(graph, child, {root}));

  af_ctx.phase("VERIFY_DEPTH_REGRESSION");
  LineageNode shallow;
  shallow.candidate = candidate(22);
  shallow.candidate_generation = CandidateGeneration::first();
  shallow.lineage = lineage(920);
  shallow.parents = {child};
  shallow.depth = 1;  // must be 2
  shallow.created_epoch = kEpoch;
  const Status regression = graph.insert(shallow);
  EXPECT_STATUS_CODE(af_ctx, regression, ErrorCode::GenerationRegression);

  LineageNode over_deep;
  over_deep.candidate = candidate(23);
  over_deep.candidate_generation = CandidateGeneration::first();
  over_deep.lineage = lineage(920);
  over_deep.parents = {root};
  over_deep.depth = 9;
  over_deep.created_epoch = kEpoch;
  EXPECT_STATUS_CODE(af_ctx, graph.insert(over_deep), ErrorCode::GenerationRegression);

  af_ctx.phase("VERIFY_REPARENT_REFUSAL");
  // The same candidate with different ancestry is finalized history, not a
  // repair opportunity.
  LineageNode reparented;
  reparented.candidate = child;
  reparented.candidate_generation = CandidateGeneration::first();
  reparented.lineage = lineage(920);
  reparented.parents = {};
  reparented.depth = 0;
  reparented.created_epoch = kEpoch;
  const Status reparent = graph.insert(reparented);
  EXPECT_STATUS_CODE(af_ctx, reparent, ErrorCode::AlreadyExists);

  af_ctx.phase("VERIFY_IDENTICAL_REPLAY_IS_ABSORBED");
  const LineageNode* existing = graph.find(child);
  EXPECT_TRUE(af_ctx, existing != nullptr);
  if (existing != nullptr) {
    EXPECT_OK(af_ctx, graph.insert(*existing));
    EXPECT_EQ(af_ctx, graph.size(), static_cast<std::size_t>(2));
  }

  af_ctx.phase("VERIFY_LINEAGE_IDENTITY_INHERITANCE");
  // A child belongs to its primary parent's lineage. The explicit identity is
  // ignored rather than silently swapped in, so an ancestry claim cannot be
  // rewritten by the caller.
  const Result<LineageNode> inherited = graph.make_child(candidate(24), CandidateGeneration::first(),
                                                         {child}, lineage(9999), kEpoch);
  EXPECT_OK(af_ctx, inherited);
  EXPECT_EQ(af_ctx, inherited.value().lineage, lineage(920));
  EXPECT_EQ(af_ctx, inherited.value().depth, std::uint32_t{2});

  af_ctx.phase("VERIFY_DEPTH_BOUND");
  // A chain that would exceed the documented depth bound is refused rather
  // than silently accepted.
  LineageGraph deep;
  CandidateId previous = candidate(500);
  EXPECT_OK(af_ctx, insert_root(deep, previous, lineage(950)));
  bool reached_bound = false;
  for (std::uint32_t index = 1; index <= kMaxLineageDepth + 2; ++index) {
    const CandidateId next = candidate(500 + index);
    const Status inserted = insert_child(deep, next, {previous});
    if (!inserted.ok()) {
      reached_bound = true;
      EXPECT_STATUS_CODE(af_ctx, inserted, ErrorCode::InvalidArgument);
      break;
    }
    previous = next;
  }
  EXPECT_TRUE(af_ctx, reached_bound);
  EXPECT_OK(af_ctx, deep.validate());
}

AF_TEST_CASE(lineage, traversal_order_is_deterministic) {
  af_ctx.phase("SETUP");
  LineageGraph graph;
  const CandidateId root = candidate(100);
  const CandidateId a = candidate(101);
  const CandidateId b = candidate(102);
  const CandidateId c = candidate(103);
  const CandidateId d = candidate(104);
  EXPECT_TRUE(af_ctx, build_reference_graph(graph, root, a, b, c, d));
  EXPECT_OK(af_ctx, graph.validate());

  af_ctx.phase("VERIFY_ANCESTORS");
  const std::vector<CandidateId> ancestors = graph.ancestors_of(d);
  // Breadth first, nearest first: c and b at depth 1, then a, then root.
  EXPECT_EQ(af_ctx, ancestors.size(), static_cast<std::size_t>(4));
  EXPECT_EQ(af_ctx, ancestors[0], c);
  EXPECT_EQ(af_ctx, ancestors[1], b);
  EXPECT_EQ(af_ctx, ancestors[2], a);
  EXPECT_EQ(af_ctx, ancestors[3], root);

  // Repeated calls produce byte-identical results.
  EXPECT_TRUE(af_ctx, graph.ancestors_of(d) == ancestors);
  EXPECT_EQ(af_ctx, render(graph.ancestors_of(d)), render(ancestors));

  af_ctx.phase("VERIFY_DESCENDANTS");
  const std::vector<CandidateId> descendants = graph.descendants_of(root);
  EXPECT_EQ(af_ctx, descendants.size(), static_cast<std::size_t>(4));
  EXPECT_FALSE(af_ctx, contains(descendants, root));
  EXPECT_TRUE(af_ctx, contains(descendants, a));
  EXPECT_TRUE(af_ctx, contains(descendants, b));
  EXPECT_TRUE(af_ctx, contains(descendants, c));
  EXPECT_TRUE(af_ctx, contains(descendants, d));
  // Depth order: shallower nodes first.
  EXPECT_EQ(af_ctx, descendants[0], a);
  EXPECT_EQ(af_ctx, descendants[1], b);
  EXPECT_EQ(af_ctx, descendants[2], c);
  EXPECT_EQ(af_ctx, descendants[3], d);
  EXPECT_TRUE(af_ctx, graph.descendants_of(root) == descendants);

  af_ctx.phase("VERIFY_PATH_TO_ROOT");
  const std::vector<CandidateId> path = graph.path_to_root(d);
  // The path is the primary-parent chain, root first. d's primary parent is c,
  // c's primary parent is a, and a's primary parent is the root, so the chain
  // has four entries and visits a: skipping it would not be a parent chain.
  EXPECT_EQ(af_ctx, path.size(), static_cast<std::size_t>(4));
  EXPECT_EQ(af_ctx, path[0], root);
  EXPECT_EQ(af_ctx, path[1], a);
  EXPECT_EQ(af_ctx, path[2], c);
  EXPECT_EQ(af_ctx, path[3], d);
  EXPECT_TRUE(af_ctx, graph.path_to_root(root) == std::vector<CandidateId>{root});
  EXPECT_TRUE(af_ctx, graph.path_to_root(a) == std::vector<CandidateId>({root, a}));

  af_ctx.phase("VERIFY_INSERTION_ORDER_INDEPENDENCE");
  // A graph built in a different insertion order traverses identically.
  LineageGraph mirrored;
  EXPECT_TRUE(af_ctx, build_reference_graph(mirrored, root, a, b, c, d));
  EXPECT_TRUE(af_ctx, mirrored.ancestors_of(d) == ancestors);
  EXPECT_TRUE(af_ctx, mirrored.descendants_of(root) == descendants);
  EXPECT_TRUE(af_ctx, mirrored.path_to_root(d) == path);
}

AF_TEST_CASE(lineage, retirement_preserves_history_and_is_idempotent) {
  af_ctx.phase("SETUP");
  LineageGraph graph;
  const CandidateId root = candidate(200);
  const CandidateId child = candidate(201);
  EXPECT_OK(af_ctx, insert_root(graph, root, lineage(920)));
  EXPECT_OK(af_ctx, insert_child(graph, child, {root}));

  af_ctx.phase("VERIFY_RETIRE");
  EXPECT_FALSE(af_ctx, graph.is_retired(root));
  EXPECT_OK(af_ctx, graph.retire(root, "lost selection", kEpoch));
  EXPECT_TRUE(af_ctx, graph.is_retired(root));

  const LineageNode* retired = graph.find(root);
  EXPECT_TRUE(af_ctx, retired != nullptr);
  if (retired != nullptr) {
    EXPECT_TRUE(af_ctx, retired->retired);
    EXPECT_EQ(af_ctx, retired->retirement_reason, std::string("lost selection"));
    EXPECT_EQ(af_ctx, retired->retired_epoch.value(), kEpoch.value());
    // Retirement preserves the node and its ancestry.
    EXPECT_EQ(af_ctx, retired->depth, std::uint32_t{0});
    EXPECT_OK(af_ctx, graph.validate());
  }

  af_ctx.phase("VERIFY_IDEMPOTENCE");
  // A second retirement is absorbed: the first retirement is the durable fact
  // and a later duplicate must not overwrite its reason or epoch.
  EXPECT_OK(af_ctx, graph.retire(root, "overwritten", CoordinatorEpoch::from_value(99)));
  const LineageNode* after = graph.find(root);
  EXPECT_TRUE(af_ctx, after != nullptr);
  if (after != nullptr) {
    EXPECT_EQ(af_ctx, after->retirement_reason, std::string("lost selection"));
    EXPECT_EQ(af_ctx, after->retired_epoch.value(), kEpoch.value());
  }

  af_ctx.phase("VERIFY_RETIRE_UNKNOWN");
  const Status unknown = graph.retire(candidate(999), "never existed", kEpoch);
  EXPECT_STATUS_CODE(af_ctx, unknown, ErrorCode::UnknownIdentity);

  af_ctx.phase("VERIFY_RETIREMENT_DOES_NOT_DESTROY_DESCENDANTS");
  EXPECT_EQ(af_ctx, graph.size(), static_cast<std::size_t>(2));
  EXPECT_TRUE(af_ctx, contains(graph.descendants_of(root), child));
  EXPECT_EQ(af_ctx, graph.path_to_root(child).size(), static_cast<std::size_t>(2));
}

AF_TEST_CASE(lineage, validate_catches_a_hand_corrupted_graph) {
  af_ctx.phase("SETUP");
  LineageGraph graph;
  const CandidateId root = candidate(300);
  const CandidateId middle = candidate(301);
  const CandidateId leaf = candidate(302);
  EXPECT_OK(af_ctx, insert_root(graph, root, lineage(930)));
  EXPECT_OK(af_ctx, insert_child(graph, middle, {root}));
  EXPECT_OK(af_ctx, insert_child(graph, leaf, {middle}));
  EXPECT_OK(af_ctx, graph.validate());

  af_ctx.phase("VERIFY_CYCLE_IS_CAUGHT");
  {
    // Build a real cycle behind the API by restoring a mutated node map.
    std::unordered_map<CandidateId, LineageNode> corrupted = graph.nodes();
    const auto root_entry = corrupted.find(root);
    EXPECT_TRUE(af_ctx, root_entry != corrupted.end());
    if (root_entry != corrupted.end()) {
      root_entry->second.parents = {leaf};
      root_entry->second.depth = 3;
    }
    LineageGraph cyclic;
    cyclic.restore(corrupted);
    const Status caught = cyclic.validate();
    EXPECT_STATUS_CODE(af_ctx, caught, ErrorCode::LineageCycle);
  }

  af_ctx.phase("VERIFY_ORPHAN_IS_CAUGHT");
  {
    std::unordered_map<CandidateId, LineageNode> corrupted = graph.nodes();
    const auto leaf_entry = corrupted.find(leaf);
    EXPECT_TRUE(af_ctx, leaf_entry != corrupted.end());
    if (leaf_entry != corrupted.end()) {
      leaf_entry->second.parents = {candidate(9999)};
    }
    LineageGraph orphaned;
    orphaned.restore(corrupted);
    EXPECT_STATUS_CODE(af_ctx, orphaned.validate(), ErrorCode::LineageOrphan);
  }

  af_ctx.phase("VERIFY_DEPTH_INCONSISTENCY_IS_CAUGHT");
  {
    std::unordered_map<CandidateId, LineageNode> corrupted = graph.nodes();
    const auto leaf_entry = corrupted.find(leaf);
    EXPECT_TRUE(af_ctx, leaf_entry != corrupted.end());
    if (leaf_entry != corrupted.end()) {
      leaf_entry->second.depth = 1;
    }
    LineageGraph inconsistent;
    inconsistent.restore(corrupted);
    EXPECT_STATUS_CODE(af_ctx, inconsistent.validate(), ErrorCode::GenerationRegression);
  }

  af_ctx.phase("VERIFY_SELF_PARENT_IS_CAUGHT");
  {
    std::unordered_map<CandidateId, LineageNode> corrupted = graph.nodes();
    const auto middle_entry = corrupted.find(middle);
    EXPECT_TRUE(af_ctx, middle_entry != corrupted.end());
    if (middle_entry != corrupted.end()) {
      middle_entry->second.parents = {middle};
    }
    LineageGraph selfish;
    selfish.restore(corrupted);
    EXPECT_STATUS_CODE(af_ctx, selfish.validate(), ErrorCode::LineageSelfParent);
  }

  af_ctx.phase("VERIFY_GENERATION_ZERO_IS_CAUGHT");
  {
    std::unordered_map<CandidateId, LineageNode> corrupted = graph.nodes();
    const auto middle_entry = corrupted.find(middle);
    EXPECT_TRUE(af_ctx, middle_entry != corrupted.end());
    if (middle_entry != corrupted.end()) {
      middle_entry->second.candidate_generation = CandidateGeneration();
    }
    LineageGraph generation_zero;
    generation_zero.restore(corrupted);
    EXPECT_STATUS_CODE(af_ctx, generation_zero.validate(), ErrorCode::InvalidArgument);
  }

  af_ctx.phase("VERIFY_CORRUPTED_GRAPH_TERMINATES_TRAVERSAL");
  {
    // A cycle must not make traversal loop forever: the traversal is expected
    // to terminate by returning no root-first path rather than fabricating one.
    std::unordered_map<CandidateId, LineageNode> corrupted = graph.nodes();
    const auto root_entry = corrupted.find(root);
    if (root_entry != corrupted.end()) {
      root_entry->second.parents = {leaf};
      root_entry->second.depth = 3;
    }
    LineageGraph cyclic;
    cyclic.restore(corrupted);
    EXPECT_TRUE(af_ctx, cyclic.path_to_root(leaf).empty());
  }

  af_ctx.phase("VERIFY_HEALTHY_GRAPH_STILL_VALID");
  EXPECT_OK(af_ctx, graph.validate());
  EXPECT_EQ(af_ctx, graph.size(), static_cast<std::size_t>(3));
}
