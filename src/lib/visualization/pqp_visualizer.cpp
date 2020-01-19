#include <memory>
#include <string>
#include <utility>

#include "constant_mappings.hpp"
#include "expression/expression_utils.hpp"
#include "expression/pqp_subquery_expression.hpp"
#include "operators/limit.hpp"
#include "operators/projection.hpp"
#include "operators/table_scan.hpp"
#include "utils/format_bytes.hpp"
#include "utils/format_duration.hpp"
#include "visualization/abstract_visualizer.hpp"
#include "visualization/pqp_visualizer.hpp"

namespace opossum {

PQPVisualizer::PQPVisualizer() = default;

PQPVisualizer::PQPVisualizer(GraphvizConfig graphviz_config, VizGraphInfo graph_info, VizVertexInfo vertex_info,
                             VizEdgeInfo edge_info)
    : AbstractVisualizer(std::move(graphviz_config), std::move(graph_info), std::move(vertex_info),
                         std::move(edge_info)) {}

void PQPVisualizer::_build_graph(const std::vector<std::shared_ptr<AbstractOperator>>& plans) {
  std::unordered_set<std::shared_ptr<const AbstractOperator>> visualized_ops;

  for (const auto& plan : plans) {
    _build_subtree(plan, visualized_ops);
  }

  {
    // Print the "Total by operator" box
    std::stringstream operator_breakdown_stream;
    operator_breakdown_stream << "{Total by operator|{";

    // Print first column (operator name)
    for (const auto& [operator_name, _] : duration_by_operator_name) {
      operator_breakdown_stream << " " << operator_name << " \\r";
    }
    operator_breakdown_stream << "total\\r";

    // Print second column (operator duration) and track total durationc
    operator_breakdown_stream << "|";
    auto total_nanoseconds = std::chrono::nanoseconds{};
    for (const auto& [_, nanoseconds] : duration_by_operator_name) {
      operator_breakdown_stream << " " << format_duration(nanoseconds) << " \\l";  // TODO add percentage
      total_nanoseconds += nanoseconds;
    }
    operator_breakdown_stream << " " << format_duration(total_nanoseconds) << " \\l";

    // Print third column (relative operator duration)
    operator_breakdown_stream << "|";
    for (const auto& [_, nanoseconds] : duration_by_operator_name) {
      operator_breakdown_stream << round(static_cast<double>(nanoseconds.count()) / total_nanoseconds.count() * 100) << " %\\l";  // TODO add percentage
    }
    operator_breakdown_stream << " \\l";

    operator_breakdown_stream << "}}";

    VizVertexInfo vertex_info = _default_vertex;
    vertex_info.shape = "record";
    vertex_info.font_name = "Monospace";  // TODO rly?
    vertex_info.label = operator_breakdown_stream.str();
    boost::add_vertex(vertex_info, _graph);
  }
}

void PQPVisualizer::_build_subtree(const std::shared_ptr<const AbstractOperator>& op,
                                   std::unordered_set<std::shared_ptr<const AbstractOperator>>& visualized_ops) {
  // Avoid drawing dataflows/ops redundantly in diamond shaped PQPs
  if (visualized_ops.find(op) != visualized_ops.end()) return;
  visualized_ops.insert(op);

  _add_operator(op);

  if (op->input_left()) {
    auto left = op->input_left();
    _build_subtree(left, visualized_ops);
    _build_dataflow(left, op, InputSide::Left);
  }

  if (op->input_right()) {
    auto right = op->input_right();
    _build_subtree(right, visualized_ops);
    _build_dataflow(right, op, InputSide::Right);
  }

  switch (op->type()) {
    case OperatorType::Projection: {
      const auto projection = std::dynamic_pointer_cast<const Projection>(op);
      for (const auto& column_expression : projection->expressions) {
        _visualize_subqueries(op, column_expression, visualized_ops);
      }
    } break;

    case OperatorType::TableScan: {
      const auto table_scan = std::dynamic_pointer_cast<const TableScan>(op);
      _visualize_subqueries(op, table_scan->predicate(), visualized_ops);
    } break;

    case OperatorType::Limit: {
      const auto limit = std::dynamic_pointer_cast<const Limit>(op);
      _visualize_subqueries(op, limit->row_count_expression(), visualized_ops);
    } break;

    default: {
    }  // OperatorType has no expressions
  }
}

void PQPVisualizer::_visualize_subqueries(const std::shared_ptr<const AbstractOperator>& op,
                                          const std::shared_ptr<AbstractExpression>& expression,
                                          std::unordered_set<std::shared_ptr<const AbstractOperator>>& visualized_ops) {
  visit_expression(expression, [&](const auto& sub_expression) {
    const auto pqp_subquery_expression = std::dynamic_pointer_cast<PQPSubqueryExpression>(sub_expression);
    if (!pqp_subquery_expression) return ExpressionVisitation::VisitArguments;

    _build_subtree(pqp_subquery_expression->pqp, visualized_ops);

    auto edge_info = _default_edge;
    auto correlated_str = std::string(pqp_subquery_expression->is_correlated() ? "correlated" : "uncorrelated");
    edge_info.label = correlated_str + " subquery";
    edge_info.style = "dashed";
    _add_edge(pqp_subquery_expression->pqp, op, edge_info);

    return ExpressionVisitation::VisitArguments;
  });
}

void PQPVisualizer::_build_dataflow(const std::shared_ptr<const AbstractOperator>& from,
                                    const std::shared_ptr<const AbstractOperator>& to, const InputSide side) {
  VizEdgeInfo info = _default_edge;

  if (const auto& output = from->get_output()) {
    std::stringstream stream;

    stream << std::to_string(output->row_count()) + " row(s)/";
    stream << std::to_string(output->chunk_count()) + " chunk(s)/";
    stream << format_bytes(output->memory_usage(MemoryUsageCalculationMode::Sampled));

    info.label = stream.str();

    info.pen_width = output->row_count();
    if (to->input_right() != nullptr) {
      info.arrowhead = side == InputSide::Left ? "lnormal" : "rnormal";
    }
  }

  _add_edge(from, to, info);
}

void PQPVisualizer::_add_operator(const std::shared_ptr<const AbstractOperator>& op) {
  VizVertexInfo info = _default_vertex;
  auto label = op->description(DescriptionMode::MultiLine);

  if (op->get_output()) {
    auto total = op->performance_data().walltime;
    label += "\n\n" + format_duration(total);
    info.pen_width = total.count();
  }

  duration_by_operator_name[op->name()] += op->performance_data().walltime;

  info.label = label;
  _add_vertex(op, info);
}

}  // namespace opossum
