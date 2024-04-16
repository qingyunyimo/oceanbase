/**
 * Copyright (c) 2021 OceanBase
 * OceanBase CE is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/recursive_cte/ob_recursive_union_op.h"
#include "sql/engine/expr/ob_expr_operator.h"
#include "sql/engine/expr/ob_datum_cast.h"


namespace oceanbase
{
using namespace common;
namespace sql
{
const int64_t ObRecursiveUnionSpec::UNUSED_POS = -2;
int ObRecursiveUnionOp::inner_close()
{
  int ret = OB_SUCCESS;
  if (OB_NOT_NULL(inner_data_)) {
    inner_data_->destroy();
  }
  return ret;
}

ObRecursiveUnionSpec::ObRecursiveUnionSpec(ObIAllocator &alloc, const ObPhyOperatorType type)
    : ObOpSpec(alloc, type),
      sort_collations_(alloc),
      cycle_by_col_lists_(alloc),
      output_union_exprs_(alloc),
      pump_operator_id_(OB_INVALID_ID),
      search_expr_(nullptr),
      cycle_expr_(nullptr),
      strategy_(ObRecursiveInnerDataOracleOp::SearchStrategyType::BREADTH_FRIST),
      cycle_value_(nullptr),
      cycle_default_value_(nullptr),
      identify_seq_offset_(-1),
      is_rcte_distinct_(false),
      hash_funcs_(alloc),
      sort_cmp_funcs_(alloc),
      deduplicate_sort_collations_(alloc)
{
}

ObRecursiveUnionSpec::~ObRecursiveUnionSpec()
{
}

OB_SERIALIZE_MEMBER((ObRecursiveUnionSpec, ObOpSpec),
                    sort_collations_,
                    cycle_by_col_lists_,
                    output_union_exprs_,
                    pump_operator_id_,
                    search_expr_,
                    cycle_expr_,
                    strategy_,
                    cycle_value_,
                    cycle_default_value_,
                    identify_seq_offset_,
                    is_rcte_distinct_,
                    hash_funcs_,
                    sort_cmp_funcs_,
                    deduplicate_sort_collations_);

int ObRecursiveUnionOp::inner_rescan()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(inner_data_->rescan())){
    LOG_WARN("Failed to rescan inner data", K(ret));
  } else if (OB_FAIL(ObOperator::inner_rescan())) {
    LOG_WARN("Operator rescan failed", K(ret));
  }
  return ret;
}

int ObRecursiveUnionOp::inner_open()
{
  int ret = OB_SUCCESS;
  void *buf = nullptr;
  ObOperatorKit *op_kit = nullptr;
  bool is_oracle_mode = lib::is_oracle_mode();

  if (is_oracle_mode) {
    if (OB_ISNULL(buf = ctx_.get_allocator().alloc(sizeof(ObRecursiveInnerDataOracleOp)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("failed to alloc memory for ObRecursiveInnerDataOracleOp", K(ret));
    } else {
      inner_data_ = new (buf) ObRecursiveInnerDataOracleOp(
        get_eval_ctx(), ctx_, MY_SPEC.output_union_exprs_, MY_SPEC.get_left()->output_,
        MY_SPEC.sort_collations_, MY_SPEC.cycle_by_col_lists_, MY_SPEC.identify_seq_offset_);
    }
  } else {
    if (OB_ISNULL(buf = ctx_.get_allocator().alloc(sizeof(ObRecursiveInnerDataMysqlOp)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("failed to alloc memory for ObRecursiveInnerDataMysqlOp", K(ret));
    } else {
      inner_data_ = new (buf) ObRecursiveInnerDataMysqlOp(
        get_eval_ctx(), ctx_, MY_SPEC.output_union_exprs_, MY_SPEC.is_rcte_distinct_,
        MY_SPEC.hash_funcs_, MY_SPEC.sort_cmp_funcs_, MY_SPEC.deduplicate_sort_collations_);

      // must set batch_size_ of inner_data_ before call inner_data_->init()
      // because it need batch_size_ to preallocate memory
      if (MY_SPEC.is_vectorized()) {
        inner_data_->set_batch_size(MY_SPEC.max_batch_size_);
      }
    }
  }

  if (OB_FAIL(ret)) {
    // return
  } else if (OB_ISNULL(left_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("Left op is null", K(ret));
  } else if (OB_FAIL(inner_data_->init(MY_SPEC.search_expr_, MY_SPEC.cycle_expr_))) {
    LOG_WARN("Failed to create hash filter", K(ret));
  } else if (OB_FAIL(is_oracle_mode
                     && cast_result(MY_SPEC.cycle_value_, MY_SPEC.cycle_expr_,
                                    &reinterpret_cast<ObRecursiveInnerDataOracleOp *>(inner_data_)
                                       ->cycle_value_))) {
    LOG_WARN("Failed to calculate cycle value", K(ret));
  } else if (OB_FAIL(is_oracle_mode
                     && cast_result(MY_SPEC.cycle_default_value_, MY_SPEC.cycle_expr_,
                                    &reinterpret_cast<ObRecursiveInnerDataOracleOp *>(inner_data_)
                                       ->non_cycle_value_))) {
    LOG_WARN("Failed to calculate non-cycle value", K(ret));
  } else if (OB_ISNULL(op_kit = ctx_.get_operator_kit(MY_SPEC.pump_operator_id_))
             || OB_ISNULL(op_kit->op_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get ObOperater from exec ctx failed", K(MY_SPEC.pump_operator_id_), K(op_kit),
             K(MY_SPEC.search_expr_), K(MY_SPEC.strategy_));
  } else {
    inner_data_->set_left_child(left_);
    inner_data_->set_right_child(right_);

    if (OB_FAIL(inner_data_->set_fake_cte_table_and_reader(
          static_cast<ObFakeCTETableOp *>(op_kit->op_)))) {
      LOG_WARN("Failed to set fake cte table and reader", K(ret));
    } else if (is_oracle_mode) {
      reinterpret_cast<ObRecursiveInnerDataOracleOp *>(inner_data_)
        ->set_search_strategy(MY_SPEC.strategy_);
    }
    LOG_DEBUG("recursive union inner data op open", K(MY_SPEC.output_), K(MY_SPEC.left_->output_),
              K(MY_SPEC.right_->output_));

    for (int64_t i = 0; OB_SUCC(ret) && i < MY_SPEC.get_left()->get_output_count(); i++) {
      const ObExpr *expr = MY_SPEC.get_left()->output_.at(i);
      if (OB_ISNULL(expr) || OB_ISNULL(expr->basic_funcs_)
          || OB_ISNULL(expr->basic_funcs_->null_first_cmp_)
          || OB_ISNULL(expr->basic_funcs_->null_last_cmp_)
          || OB_ISNULL(expr->basic_funcs_->default_hash_)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("left output expr is null or basic_funcs_ is null", K(ret));
      }
    }
  }

  return ret;
}

int ObRecursiveUnionOp::inner_get_next_row()
{
  int ret = OB_SUCCESS;
  clear_evaluated_flag();
  if (OB_FAIL(try_check_status())) {
    LOG_WARN("Failed to check physical plan status", K(ret));
  } else if (OB_FAIL(inner_data_->get_next_row())) {
    if (OB_ITER_END != ret) {
      LOG_WARN("Failed to get next sort row from recursive inner data", K(ret));
    }
  }
  return ret;
}

int ObRecursiveUnionOp::inner_get_next_batch(const int64_t max_row_cnt)
{
  int ret = OB_SUCCESS;
  clear_evaluated_flag();
  int64_t batch_size = std::min(max_row_cnt, MY_SPEC.max_batch_size_);
  if (OB_FAIL(try_check_status())) {
    LOG_WARN("Failed to check physical plan status", K(ret));
  } else if (OB_FAIL(inner_data_->get_next_batch(batch_size, brs_))) {
    LOG_WARN("Failed to get next sort row from recursive inner data", K(ret));
  }
  return ret;
}

int ObRecursiveUnionSpec::set_cycle_pseudo_values(ObExpr *v, ObExpr *d_v) {
  int ret = OB_SUCCESS;
  cycle_value_ = v;
  cycle_default_value_ = d_v;
  return ret;
}

int ObRecursiveUnionOp::cast_result(const ObExpr *src_expr,
                                       const ObExpr *dst_expr,
                                       ObDatum *expr_datum)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(src_expr)) {
    // do nothing
  } else if (src_expr->datum_meta_.type_ == dst_expr->datum_meta_.type_) {
    ObDatum *src_value = NULL;
    if (OB_FAIL(src_expr->eval(eval_ctx_, src_value))) {
      LOG_WARN("Failed to calculate cycle value", K(ret));
    } else if (OB_FAIL(expr_datum->deep_copy(*src_value, ctx_.get_allocator()))) {
      LOG_WARN("deep copy datum failed", K(ret));
    }
  } else if (OB_ISNULL(eval_ctx_.datum_caster_) && OB_FAIL(eval_ctx_.init_datum_caster())) {
    LOG_WARN("init datum caster failed", K(ret));
  } else {
    ObCastMode cm = CM_NONE;
    ObDatum *cast_datum = NULL;
    if (OB_ISNULL(ctx_.get_my_session())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("sql session info is null", K(ret));
    } else if (OB_FAIL(ObSQLUtils::get_default_cast_mode(ctx_.get_my_session(), cm))) {
      LOG_WARN("Failed to get default cast mode", K(ret));
    } else if (OB_FAIL(eval_ctx_.datum_caster_->to_type(
        dst_expr->datum_meta_, *src_expr, cm, cast_datum, eval_ctx_.get_batch_idx()))) {
      LOG_WARN("fail to dynamic cast", K(ret));
    } else if (OB_FAIL(expr_datum->deep_copy(*cast_datum, ctx_.get_allocator()))) {
      LOG_WARN("deep copy datum failed", K(ret));
    }
  }
  return ret;
}

}  // namespace sql
}  // namespace oceanbase