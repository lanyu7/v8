// Copyright 2014 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <functional>
#include <limits>

#include "src/compiler/graph.h"
#include "src/compiler/graph-reducer.h"
#include "src/compiler/node.h"
#include "src/compiler/node-properties.h"
#include "src/compiler/verifier.h"

namespace v8 {
namespace internal {
namespace compiler {

enum class GraphReducer::State : uint8_t {
  kUnvisited,
  kRevisit,
  kOnStack,
  kVisited
};


void Reducer::Finalize() {}

GraphReducer::GraphReducer(Zone* zone, Graph* graph,
                           CommonOperatorBuilder* common, Node* dead)
    : graph_(graph),
      common_(common),
      dead_(dead),
      state_(graph, 4),
      reducers_(zone),
      revisit_(zone),
      stack_(zone),
      nb_traversed_uses_(0),
      nb_visited_nodes_(0),
      revisit_all_nodes_(false) {
  if (dead != nullptr) {
    NodeProperties::SetType(dead_, Type::None());
  }
}

GraphReducer::~GraphReducer() {}


void GraphReducer::AddReducer(Reducer* reducer) {
  reducers_.push_back(reducer);
}


void GraphReducer::ReduceNode(Node* node) {
  DCHECK(stack_.empty());
  DCHECK(revisit_.empty());
  Push(node);
  for (;;) {
    if (!stack_.empty()) {
      // Process the node on the top of the stack, potentially pushing more or
      // popping the node off the stack.
      ReduceTop();
    } else if (!revisit_.empty()) {
      // If the stack becomes empty, revisit any nodes in the revisit queue.
      Node* const node = revisit_.front();
      revisit_.pop();
      if (state_.Get(node) == State::kRevisit) {
        // state can change while in queue.
        Push(node);
      }
    } else if (update_and_get_revisit_all_nodes(nb_visited_nodes())) {
      // We revisit the graph again due to the
      // turbo_revisit_whole_graph_threshold flag.
      set_revisit_all_nodes(false);
      reset_nb_traversed_uses();
      reset_nb_visited_nodes();
      state_.reset(graph_);
      Push(graph()->end());
    } else {
      // Run all finalizers.
      for (Reducer* const reducer : reducers_) reducer->Finalize();

      // Check if we have new nodes to revisit.
      if (revisit_.empty()) break;
    }
  }
  DCHECK(revisit_.empty());
  DCHECK(stack_.empty());
}

void GraphReducer::ReduceGraph() {
  set_revisit_all_nodes(false);
  reset_nb_traversed_uses();
  reset_nb_visited_nodes();
  ReduceNode(graph()->end());
}

Reduction GraphReducer::Reduce(Node* const node) {
  auto skip = reducers_.end();
  for (auto i = reducers_.begin(); i != reducers_.end();) {
    if (i != skip) {
      Reduction reduction = (*i)->Reduce(node);
      if (!reduction.Changed()) {
        // No change from this reducer.
      } else if (reduction.replacement() == node) {
        // {replacement} == {node} represents an in-place reduction. Rerun
        // all the other reducers for this node, as now there may be more
        // opportunities for reduction.
        if (FLAG_trace_turbo_reduction) {
          OFStream os(stdout);
          os << "- In-place update of " << *node << " by reducer "
             << (*i)->reducer_name() << std::endl;
        }
        skip = i;
        i = reducers_.begin();
        continue;
      } else {
        // {node} was replaced by another node.
        if (FLAG_trace_turbo_reduction) {
          OFStream os(stdout);
          os << "- Replacement of " << *node << " with "
             << *(reduction.replacement()) << " by reducer "
             << (*i)->reducer_name() << std::endl;
        }
        return reduction;
      }
    }
    ++i;
  }
  if (skip == reducers_.end()) {
    // No change from any reducer.
    return Reducer::NoChange();
  }
  // At least one reducer did some in-place reduction.
  return Reducer::Changed(node);
}


void GraphReducer::ReduceTop() {
  DCHECK(!stack_.empty());
  NodeState& entry = stack_.top();
  Node* node = entry.node;
  DCHECK(state_.Get(node) == State::kOnStack);

  if (node->IsDead()) return Pop();  // Node was killed while on stack.

  Node::Inputs node_inputs = node->inputs();

  // Recurse on an input if necessary.
  int start = entry.input_index < node_inputs.count() ? entry.input_index : 0;
  for (int i = start; i < node_inputs.count(); ++i) {
    Node* input = node_inputs[i];
    // If we are the use of a placeholder, then we rewire ourself to our actual
    // parent.
    if (input->opcode() == IrOpcode::kReplacementPlaceholder) {
      while (input->opcode() == IrOpcode::kReplacementPlaceholder) {
        input = input->InputAt(
            0);  // TODO(leszeks) check input type and take correct input.
      }
      node->ReplaceInput(i, input);
    }
    if (input != node && Recurse(input)) {
      entry.input_index = i + 1;
      return;
    }
  }
  for (int i = 0; i < start; ++i) {
    Node* input = node_inputs[i];

    if (input->opcode() == IrOpcode::kReplacementPlaceholder) {
      while (input->opcode() == IrOpcode::kReplacementPlaceholder) {
        input = input->InputAt(
            0);  // TODO(leszeks) check input type and take correct input.
      }
      node->ReplaceInput(i, input);
    }

    if (input != node && Recurse(input)) {
      entry.input_index = i + 1;
      return;
    }
  }

  // The placeholder node cannot be reduced.
  if (node->opcode() == IrOpcode::kReplacementPlaceholder) return Pop();

  // Remember the max node id before reduction.
  NodeId const max_id = static_cast<NodeId>(graph()->NodeCount() - 1);

  // All inputs should be visited or on stack. Apply reductions to node.
  Reduction reduction = Reduce(node);

  // If there was no reduction, pop {node} and continue.
  if (!reduction.Changed()) return Pop();

  // Check if the reduction is an in-place update of the {node}.
  Node* const replacement = reduction.replacement();

  if (replacement == node) {
    // In-place update of {node}, may need to recurse on an input.
    Node::Inputs node_inputs = node->inputs();
    for (int i = 0; i < node_inputs.count(); ++i) {
      Node* input = node_inputs[i];
      if (input != node && Recurse(input)) {
        entry.input_index = i + 1;
        return;
      }
    }
  }

  // After reducing the node, pop it off the stack.
  Pop();

  // Check if we have a new replacement.
  if (replacement != node) {
    Replace(node, replacement, max_id);
  } else if (!update_and_get_revisit_all_nodes(nb_visited_nodes())) {
    // Always goes here if it's an in-place replacement and the
    // turbo_revisit_whole_graph_threshold is 100.
    // Revisit all uses of the node.
    for (Node* const user : node->uses()) {
      // Don't revisit this node if it refers to itself.
      if (user != node) Revisit(user);
    }
  }
}


void GraphReducer::Replace(Node* node, Node* replacement) {
  Replace(node, replacement, std::numeric_limits<NodeId>::max());
}


void GraphReducer::Replace(Node* node, Node* replacement, NodeId max_id) {
  if (node == graph()->start()) graph()->SetStart(replacement);
  if (node == graph()->end()) graph()->SetEnd(replacement);
  if (replacement->id() <= max_id) {
    if (FLAG_turbo_reduction_placeholder &&
        update_and_get_revisit_all_nodes(nb_visited_nodes())) {
      // We replace change {node} to be a placeholder, and link it to
      // {replacement}, so that the rewiring of {node}'s users will be done as
      // lazy as possible.
      int has_value_output = replacement->op()->ValueOutputCount() ? 1 : 0;
      int has_effect_output = replacement->op()->EffectOutputCount() ? 1 : 0;
      int has_control_output = replacement->op()->ControlOutputCount() ? 1 : 0;
      int nb_total_output =
          has_value_output + has_effect_output + has_control_output;
      if (!nb_total_output || node->raw_uses().empty()) {
        // We assume {node} only has itself as uses. Otherwise a DCHECK in kill
        // will fail.
        node->Kill();
        return;
      }
      node->TrimInputCount(0);
      Node* new_input = replacement;
      while (new_input->opcode() == IrOpcode::kReplacementPlaceholder) {
        new_input = new_input->InputAt(0);
      }
      DCHECK_NE(new_input->opcode(), IrOpcode::kReplacementPlaceholder);
      for (int i = 0; i < nb_total_output; i++) {
        node->AppendInput(graph()->zone(), new_input);
      }
      node->set_op(common()->ReplacementPlaceholder(
          has_value_output, has_effect_output, has_control_output));

    } else {
      for (Edge edge : node->use_edges()) {
        Node* const user = edge.from();
        Verifier::VerifyEdgeInputReplacement(edge, replacement);
        edge.UpdateTo(replacement);
        // Don't revisit this node if it refers to itself.
        if (user != node) Revisit(user);
      }
      node->Kill();
    }

  } else {
    // Replace all old uses of {node} with {replacement}, but allow new nodes
    // created by this reduction to use {node}.
    for (Edge edge : node->use_edges()) {
      Node* const user = edge.from();
      if (user->id() <= max_id) {
        edge.UpdateTo(replacement);
        // Don't revisit this node if it refers to itself.
        if (user != node) Revisit(user);
      }
    }
    // Unlink {node} if it's no longer used.
    if (node->raw_uses().empty()) node->Kill();

    // If there was a replacement, reduce it after popping {node}.
    Recurse(replacement);
  }
}


void GraphReducer::ReplaceWithValue(Node* node, Node* value, Node* effect,
                                    Node* control) {
  if (effect == nullptr && node->op()->EffectInputCount() > 0) {
    effect = NodeProperties::GetEffectInput(node);
  }
  if (control == nullptr && node->op()->ControlInputCount() > 0) {
    control = NodeProperties::GetControlInput(node);
  }

  // Requires distinguishing between value, effect and control edges.
  for (Edge edge : node->use_edges()) {
    Node* const user = edge.from();
    DCHECK(!user->IsDead());
    if (NodeProperties::IsControlEdge(edge)) {
      if (user->opcode() == IrOpcode::kIfSuccess) {
        Replace(user, control);
      } else if (user->opcode() == IrOpcode::kIfException) {
        DCHECK_NOT_NULL(dead_);
        edge.UpdateTo(dead_);
        Revisit(user);
      } else {
        DCHECK_NOT_NULL(control);
        edge.UpdateTo(control);
        Revisit(user);
      }
    } else if (NodeProperties::IsEffectEdge(edge)) {
      DCHECK_NOT_NULL(effect);
      edge.UpdateTo(effect);
      Revisit(user);
    } else {
      DCHECK_NOT_NULL(value);
      edge.UpdateTo(value);
      Revisit(user);
    }
  }
}


void GraphReducer::Pop() {
  Node* node = stack_.top().node;
  state_.Set(node, State::kVisited);
  nb_visited_nodes_++;
  stack_.pop();
}


void GraphReducer::Push(Node* const node) {
  DCHECK(state_.Get(node) != State::kOnStack);
  state_.Set(node, State::kOnStack);
  stack_.push({node, 0});
}


bool GraphReducer::Recurse(Node* node) {
  if (state_.Get(node) > State::kRevisit) return false;
  Push(node);
  return true;
}


void GraphReducer::Revisit(Node* node) {
  if (state_.Get(node) == State::kVisited) {
    incr_nb_traversed_uses();
    state_.Set(node, State::kRevisit);
    revisit_.push(node);
  }
}

}  // namespace compiler
}  // namespace internal
}  // namespace v8
