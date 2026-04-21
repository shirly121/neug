/**
 * Copyright 2020 Alibaba Group Holding Limited.
 *
 * Licensed under the Apache License, Version 2.0 (the License);
 * you may not use this file except in compliance with the License.
 */

#include "neug/compiler/function/gds/gds_algo_function.h"

#include "neug/compiler/binder/binder.h"
#include "neug/compiler/common/types/types.h"
#include "neug/compiler/common/types/value/nested.h"
#include "neug/compiler/function/table/table_function.h"
#include "neug/compiler/graph/graph_entry.h"
#include "neug/compiler/main/client_context.h"
#include "neug/execution/common/context.h"
#include "neug/utils/exception/exception.h"

namespace neug {
namespace function {

namespace {

static common::case_insensitive_map_t<std::string> extractStringOptions(
    const common::Value& value) {
  common::case_insensitive_map_t<std::string> out;
  const auto typeId = value.getDataType().getLogicalTypeID();
  if (typeId == common::LogicalTypeID::STRUCT) {
    for (auto i = 0u; i < common::StructType::getNumFields(value.getDataType());
         ++i) {
      auto& field = common::StructType::getField(value.getDataType(), i);
      const auto* child = common::NestedVal::getChildVal(&value, i);
      out.emplace(field.getName(), child->toString());
    }
    return out;
  }
  if (typeId == common::LogicalTypeID::MAP) {
    for (auto i = 0u; i < common::NestedVal::getChildrenSize(&value); ++i) {
      const auto& entry = *common::NestedVal::getChildVal(&value, i);
      const auto& entryType = entry.getDataType();
      if (entryType.getLogicalTypeID() != common::LogicalTypeID::STRUCT) {
        THROW_BINDER_EXCEPTION(
            "GDS options map entries must be structs (key/value pairs).");
      }
      if (common::StructType::getNumFields(entryType) != 2) {
        THROW_BINDER_EXCEPTION(
            "GDS options map entry must have exactly two fields.");
      }
      const auto& keyVal = *common::NestedVal::getChildVal(&entry, 0);
      const auto& valVal = *common::NestedVal::getChildVal(&entry, 1);
      keyVal.validateType(common::LogicalTypeID::STRING);
      out.emplace(keyVal.getValue<std::string>(), valVal.toString());
    }
    return out;
  }
  THROW_BINDER_EXCEPTION(
      "Second argument to GDS CALL must be a map literal, got " +
      value.getDataType().toString() + ".");
}

}  // namespace

static std::unique_ptr<TableFuncBindData> bindGDSFunction(
    main::ClientContext* clientContext, const TableFuncBindInput* input) {
  if (input->binder == nullptr) {
    THROW_BINDER_EXCEPTION("Internal error: binder not set for GDS CALL.");
  }
  auto& binder = *input->binder;
  auto graphName = input->getLiteralVal<std::string>(0);
  auto& graphEntrySet = clientContext->getGraphEntrySetUnsafe();
  graphEntrySet.validateGraphExist(graphName);
  const auto& parsed = graphEntrySet.getEntry(graphName);
  auto graphEntry = graph::GDSFunction::bindGraphEntry(*clientContext, parsed);
  auto options = extractStringOptions(input->getValue(1));
  return std::make_unique<GDSFuncBindData>(
      input->getColumns(), input->getNumRows(), input->getParams(),
      std::move(graphEntry), std::move(options));
}

GDSAlgoFunction::GDSAlgoFunction(std::string name,
                                 std::vector<common::LogicalTypeID> inputTypes,
                                 call_output_columns outputColumns)
    : NeugCallFunction(std::move(name), std::move(inputTypes),
                       std::move(outputColumns)) {
  auto* tableFn = static_cast<TableFunction*>(this);
  tableFn->bindFunc = bindGDSFunction;
}

GDSFuncBindData::GDSFuncBindData(
    binder::expression_vector columns, common::row_idx_t numRows,
    binder::expression_vector params, graph::GraphEntry graphEntryIn,
    common::case_insensitive_map_t<std::string> optionsIn,
    std::string algoNameIn)
    : TableFuncBindData(std::move(columns), numRows, std::move(params)),
      graphEntry(std::move(graphEntryIn)),
      options(std::move(optionsIn)),
      algoName(std::move(algoNameIn)) {}

GDSFuncBindData::GDSFuncBindData(const GDSFuncBindData& other)
    : TableFuncBindData(other),
      graphEntry(other.graphEntry),
      options(other.options),
      algoName(other.algoName) {}

std::unique_ptr<TableFuncBindData> GDSFuncBindData::copy() const {
  return std::make_unique<GDSFuncBindData>(*this);
}

}  // namespace function
}  // namespace neug
