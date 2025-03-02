//--------------------------------------------------------------------------------------------------
// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//
//--------------------------------------------------------------------------------------------------

#include "yb/yql/pggate/pg_select.h"
#include "yb/yql/pggate/util/pg_doc_data.h"
#include "yb/client/yb_op.h"
#include "yb/docdb/primitive_value.h"

namespace yb {
namespace pggate {

using std::make_shared;

//--------------------------------------------------------------------------------------------------
// PgSelect
//--------------------------------------------------------------------------------------------------

PgSelect::PgSelect(PgSession::ScopedRefPtr pg_session, const PgObjectId& table_id)
    : PgDml(std::move(pg_session), table_id) {
}

PgSelect::~PgSelect() {
}

void PgSelect::UseIndex(const PgObjectId& index_id) {
  index_id_ = index_id;
}

Status PgSelect::LoadIndex() {
  index_desc_ = VERIFY_RESULT(pg_session_->LoadTable(index_id_));
  return Status::OK();
}

Status PgSelect::Prepare(PreventRestart prevent_restart) {
  RETURN_NOT_OK(LoadTable());
  if (index_id_.IsValid()) {
    RETURN_NOT_OK(LoadIndex());
  }

  // Allocate READ/SELECT operation.
  auto doc_op = make_shared<PgDocReadOp>(
      pg_session_, prevent_restart, table_desc_->NewPgsqlSelect());
  read_req_ = doc_op->read_op()->mutable_request();
  if (index_id_.IsValid()) {
    index_req_ = read_req_->mutable_index_request();
    index_req_->set_table_id(index_id_.GetYBTableId());
  }

  PrepareColumns();

  // Preparation complete.
  doc_op_ = doc_op;
  return Status::OK();
}

void PgSelect::PrepareColumns() {
  // When reading, only values of partition columns are special-cased in protobuf.
  // Because DocDB API requires that partition columns must be listed in their created-order, the
  // slots for partition column bind expressions are allocated here in correct order.
  if (index_id_.IsValid()) {
    for (PgColumn &col : index_desc_->columns()) {
      col.AllocPrimaryBindPB(index_req_);
    }

    // Select ybbasectid column from the index to fetch the rows from the base table.
    PgColumn *col;
    CHECK_OK(FindIndexColumn(static_cast<int>(PgSystemAttrNum::kYBIdxBaseTupleId), &col));
    AllocIndexTargetPB()->set_column_id(col->id());
    col->set_read_requested(true);

  } else {
    for (PgColumn &col : table_desc_->columns()) {
      col.AllocPrimaryBindPB(read_req_);
    }
  }
}

//--------------------------------------------------------------------------------------------------
// DML support.
// TODO(neil) WHERE clause is not yet supported. Revisit this function when it is.

PgsqlExpressionPB *PgSelect::AllocColumnBindPB(PgColumn *col) {
  return col->AllocBindPB(read_req_);
}

PgsqlExpressionPB *PgSelect::AllocColumnBindConditionExprPB(PgColumn *col) {
  return col->AllocBindConditionExprPB(read_req_);
}

PgsqlExpressionPB *PgSelect::AllocIndexColumnBindPB(PgColumn *col) {
  return col->AllocBindPB(index_req_);
}

PgsqlExpressionPB *PgSelect::AllocColumnAssignPB(PgColumn *col) {
  // SELECT statement should not have an assign expression (SET clause).
  LOG(FATAL) << "Pure virtual function is being call";
  return nullptr;
}

PgsqlExpressionPB *PgSelect::AllocTargetPB() {
  return read_req_->add_targets();
}

PgsqlExpressionPB *PgSelect::AllocIndexTargetPB() {
  return index_req_->add_targets();
}

//--------------------------------------------------------------------------------------------------
// RESULT SET SUPPORT.
// For now, selected expressions are just a list of column names (ref).
//   SELECT column_l, column_m, column_n FROM ...

Status PgSelect::DeleteEmptyPrimaryBinds() {
  // Either ybctid or hash/primary key must be present.
  if (!ybctid_bind_) {
    PgTableDesc::ScopedRefPtr table_desc = index_desc_ ? index_desc_ : table_desc_;
    PgsqlReadRequestPB *read_req = index_desc_ ? index_req_ : read_req_;

    bool miss_partition_columns = false;
    bool has_partition_columns = false;

    for (size_t i = 0; i < table_desc->num_hash_key_columns(); i++) {
      PgColumn &col = table_desc->columns()[i];
      if (expr_binds_.find(col.bind_pb()) == expr_binds_.end()) {
        miss_partition_columns = true;
      } else {
        has_partition_columns = true;
      }
    }

    if (miss_partition_columns) {
      VLOG(1) << "Full scan is needed";
      DCHECK(table_desc.get() != index_desc_.get())
          << "Full scan should be applied to base table only";
      read_req->clear_partition_column_values();
      read_req->clear_range_column_values();
    }

    if (has_partition_columns && miss_partition_columns) {
      return STATUS(InvalidArgument, "Partition key must be fully specified");
    }

    bool miss_range_columns = false;
    size_t num_bound_range_columns = 0;

    for (size_t i = table_desc->num_hash_key_columns(); i < table_desc->num_key_columns(); i++) {
      PgColumn &col = table_desc->columns()[i];
      if (expr_binds_.find(col.bind_pb()) == expr_binds_.end()) {
        miss_range_columns = true;
      } else if (miss_range_columns) {
        return STATUS(InvalidArgument,
                      "Unspecified range key column must be at the end of the range key");
      } else {
        num_bound_range_columns++;
      }
    }

    auto *range_column_values = read_req->mutable_range_column_values();
    range_column_values->DeleteSubrange(num_bound_range_columns,
                                        range_column_values->size() - num_bound_range_columns);
  } else {
    read_req_->clear_partition_column_values();
    read_req_->clear_range_column_values();
  }

  return Status::OK();
}

//--------------------------------------------------------------------------------------------------

Status PgSelect::FindIndexColumn(int attr_num, PgColumn **col) {
  *col = VERIFY_RESULT(index_desc_->FindColumn(attr_num));
  return Status::OK();
}

Status PgSelect::BindIndexColumn(int attr_num, PgExpr *attr_value) {
  // Find column.
  PgColumn *col = nullptr;
  RETURN_NOT_OK(FindIndexColumn(attr_num, &col));

  // Check datatype.
  SCHECK_EQ(col->internal_type(), attr_value->internal_type(), Corruption,
            "Attribute value type does not match column type");

  // Alloc the protobuf.
  PgsqlExpressionPB *bind_pb = col->bind_pb();
  if (bind_pb == nullptr) {
    bind_pb = AllocIndexColumnBindPB(col);
  } else {
    if (expr_binds_.find(bind_pb) != expr_binds_.end()) {
      return STATUS_SUBSTITUTE(InvalidArgument,
                               "Index column $0 is already bound to another value", attr_num);
    }
  }

  // Link the expression and protobuf. During execution, expr will write result to the pb.
  RETURN_NOT_OK(attr_value->PrepareForRead(this, bind_pb));

  // Link the given expression "attr_value" with the allocated protobuf. Note that except for
  // constants and place_holders, all other expressions can be setup just one time during prepare.
  // Examples:
  // - Bind values for primary columns in where clause.
  //     WHERE hash = ?
  // - Bind values for a column in INSERT statement.
  //     INSERT INTO a_table(hash, key, col) VALUES(?, ?, ?)
  expr_binds_[bind_pb] = attr_value;
  return Status::OK();
}

Status PgSelect::Exec(const PgExecParameters *exec_params) {
  // Delete key columns that are not bound to any values.
  RETURN_NOT_OK(DeleteEmptyPrimaryBinds());

  // Update bind values for constants and placeholders.
  RETURN_NOT_OK(UpdateBindPBs());

  // Set execution control parameters.
  doc_op_->SetExecParams(exec_params);

  // Set column references in protobuf and whether query is aggregate.
  SetColumnRefIds(table_desc_, read_req_->mutable_column_refs());
  read_req_->set_is_aggregate(has_aggregate_targets());
  if (index_id_.IsValid()) {
    SetColumnRefIds(index_desc_, index_req_->mutable_column_refs());
    DCHECK(!has_aggregate_targets()) << "Aggregate pushdown should not happen with index";
  }

  // Execute select statement asynchronously.
  SCHECK_EQ(VERIFY_RESULT(doc_op_->Execute()), RequestSent::kTrue, IllegalState,
            "YSQL read operation was not sent");

  return Status::OK();
}

Status PgSelect::BindColumnCondEq(int attr_num, PgExpr *attr_value) {
  // Find column.
  PgColumn *col = nullptr;
  RETURN_NOT_OK(FindColumn(attr_num, &col));

  // Check datatype.
  if (attr_value) {
    SCHECK_EQ(col->internal_type(), attr_value->internal_type(), Corruption,
              "Attribute value type does not match column type");
  }

  // Alloc the protobuf.
  PgsqlExpressionPB *condition_expr_pb = AllocColumnBindConditionExprPB(col);

  if (attr_value != nullptr) {
    condition_expr_pb->mutable_condition()->set_op(QL_OP_EQUAL);

    auto op1_pb = condition_expr_pb->mutable_condition()->add_operands();
    auto op2_pb = condition_expr_pb->mutable_condition()->add_operands();

    op1_pb->set_column_id(attr_num - 1);

    RETURN_NOT_OK(attr_value->Eval(this, op2_pb->mutable_value()));
  }

  if (attr_num == static_cast<int>(PgSystemAttrNum::kYBTupleId)) {
    CHECK(attr_value->is_constant()) << "Column ybctid must be bound to constant";
    ybctid_bind_ = true;
  }

  return Status::OK();
}

Status PgSelect::BindColumnCondBetween(int attr_num, PgExpr *attr_value, PgExpr *attr_value_end) {
  // Find column.
  PgColumn *col = nullptr;
  RETURN_NOT_OK(FindColumn(attr_num, &col));

  // Check datatype.
  if (attr_value) {
    SCHECK_EQ(col->internal_type(), attr_value->internal_type(), Corruption,
              "Attribute value type does not match column type");
  }

  if (attr_value_end) {
    SCHECK_EQ(col->internal_type(), attr_value_end->internal_type(), Corruption,
              "Attribute value type does not match column type");
  }

  // Alloc the protobuf.
  PgsqlExpressionPB *condition_expr_pb = AllocColumnBindConditionExprPB(col);

  if (attr_value != nullptr) {
    if (attr_value_end != nullptr) {
      condition_expr_pb->mutable_condition()->set_op(QL_OP_BETWEEN);

      auto op1_pb = condition_expr_pb->mutable_condition()->add_operands();
      auto op2_pb = condition_expr_pb->mutable_condition()->add_operands();
      auto op3_pb = condition_expr_pb->mutable_condition()->add_operands();

      op1_pb->set_column_id(attr_num - 1);

      RETURN_NOT_OK(attr_value->Eval(this, op2_pb->mutable_value()));
      RETURN_NOT_OK(attr_value_end->Eval(this, op3_pb->mutable_value()));
    } else {
      condition_expr_pb->mutable_condition()->set_op(QL_OP_GREATER_THAN_EQUAL);

      auto op1_pb = condition_expr_pb->mutable_condition()->add_operands();
      auto op2_pb = condition_expr_pb->mutable_condition()->add_operands();

      op1_pb->set_column_id(attr_num - 1);

      RETURN_NOT_OK(attr_value->Eval(this, op2_pb->mutable_value()));
    }
  } else {
    if (attr_value_end != nullptr) {
      condition_expr_pb->mutable_condition()->set_op(QL_OP_LESS_THAN_EQUAL);

      auto op1_pb = condition_expr_pb->mutable_condition()->add_operands();
      auto op2_pb = condition_expr_pb->mutable_condition()->add_operands();

      op1_pb->set_column_id(attr_num - 1);

      RETURN_NOT_OK(attr_value_end->Eval(this, op2_pb->mutable_value()));
    } else {
      // Unreachable.
    }
  }

  if (attr_num == static_cast<int>(PgSystemAttrNum::kYBTupleId)) {
    CHECK(attr_value->is_constant()) << "Column ybctid must be bound to constant";
    CHECK(attr_value_end->is_constant()) << "Column ybctid must be bound to constant";
    ybctid_bind_ = true;
  }

  return Status::OK();
}

Status PgSelect::BindColumnCondIn(int attr_num, int n_attr_values, PgExpr **attr_values) {
  // Find column.
  PgColumn *col = nullptr;
  RETURN_NOT_OK(FindColumn(attr_num, &col));

  // Check datatype.
  // TODO(neil) Current code combine TEXT and BINARY datatypes into ONE representation.  Once that
  // is fixed, we can remove the special if() check for BINARY type.
  if (col->internal_type() != InternalType::kBinaryValue) {
    for (int i = 0; i < n_attr_values; i++) {
      if (attr_values[i]) {
        SCHECK_EQ(col->internal_type(), attr_values[i]->internal_type(), Corruption,
            "Attribute value type does not match column type");
      }
    }
  }

  // Alloc the protobuf.
  PgsqlExpressionPB *condition_expr_pb = AllocColumnBindConditionExprPB(col);

  condition_expr_pb->mutable_condition()->set_op(QL_OP_IN);

  auto op1_pb = condition_expr_pb->mutable_condition()->add_operands();
  auto op2_pb = condition_expr_pb->mutable_condition()->add_operands();

  op1_pb->set_column_id(attr_num - 1);

  for (int i = 0; i < n_attr_values; i++) {
    // Link the given expression "attr_value" with the allocated protobuf. Note that except for
    // constants and place_holders, all other expressions can be setup just one time during prepare.
    // Examples:
    // - Bind values for primary columns in where clause.
    //     WHERE hash = ?
    // - Bind values for a column in INSERT statement.
    //     INSERT INTO a_table(hash, key, col) VALUES(?, ?, ?)

    if (attr_values[i]) {
      RETURN_NOT_OK(attr_values[i]->Eval(this,
            op2_pb->mutable_value()->mutable_list_value()->add_elems()));
    }

    if (attr_num == static_cast<int>(PgSystemAttrNum::kYBTupleId)) {
      CHECK(attr_values[i]->is_constant()) << "Column ybctid must be bound to constant";
      ybctid_bind_ = true;
    }
  }

  return Status::OK();
}

}  // namespace pggate
}  // namespace yb
