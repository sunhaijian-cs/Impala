// Copyright (c) 2011 Cloudera, Inc. All rights reserved.

#include "exec/aggregation-node.h"

#include <sstream>
#include <boost/functional/hash.hpp>

#include "exprs/agg-expr.h"
#include "exprs/expr.h"
#include "runtime/descriptors.h"
#include "runtime/mem-pool.h"
#include "runtime/raw-value.h"
#include "runtime/row-batch.h"
#include "runtime/runtime-state.h"
#include "runtime/tuple.h"
#include "runtime/tuple-row.h"
#include "util/debug-util.h"
#include "util/runtime-profile.h"

#include "gen-cpp/Exprs_types.h"
#include "gen-cpp/PlanNodes_types.h"

using namespace impala;
using namespace std;
using namespace boost;

// This object appends n-int32s to the end of a normal tuple object to maintain the lengths
// of the string buffers in the tuple.
namespace impala {

class AggregationTuple {
 public:
  static AggregationTuple* Create(int tuple_size, int num_string_slots, MemPool* pool) {
    int size = tuple_size + sizeof(int32_t) * num_string_slots;
    AggregationTuple* result = reinterpret_cast<AggregationTuple*>(pool->Allocate(size));
    result->Init(size);
    return result;
  }

  void Init(int size) {
    bzero(this, size);
  }
    
  Tuple* tuple() { return reinterpret_cast<Tuple*>(this); }

  int32_t* BufferLengths(int tuple_size) {
    char* data = reinterpret_cast<char*>(this) + tuple_size;
    return reinterpret_cast<int*>(data);
  }
 private:
  void* data_;
};

}

// TODO: pass in maximum size; enforce by setting limit in mempool
// TODO: have a Status ExecNode::Init(const TPlanNode&) member function
// that does initialization outside of c'tor, so we can indicate errors
AggregationNode::AggregationNode(ObjectPool* pool, const TPlanNode& tnode,
                                 const DescriptorTbl& descs)
  : ExecNode(pool, tnode, descs),
    hash_fn_(this),
    equals_fn_(this),
    agg_tuple_id_(tnode.agg_node.agg_tuple_id),
    agg_tuple_desc_(NULL),
    singleton_output_tuple_(NULL),
    current_row_(NULL),
    num_string_slots_(0),
    tuple_pool_(new MemPool()) {
  // ignore return status for now
  Expr::CreateExprTrees(pool, tnode.agg_node.grouping_exprs, &grouping_exprs_);
  Expr::CreateExprTrees(pool, tnode.agg_node.aggregate_exprs, &aggregate_exprs_);
}

void AggregationNode::GroupingExprHash::Init(
    TupleDescriptor* agg_tuple_d, const vector<Expr*>& grouping_exprs) {
  agg_tuple_desc_ = agg_tuple_d;
  grouping_exprs_ = &grouping_exprs;
}

size_t AggregationNode::GroupingExprHash::operator()(Tuple* const& t) const {
  size_t seed = 0;
  for (int i = 0; i < grouping_exprs_->size(); ++i) {
    SlotDescriptor* slot_d = agg_tuple_desc_->slots()[i];
    const void* value;
    if (t != NULL) {
      if (t->IsNull(slot_d->null_indicator_offset())) {
        value = NULL;
      } else {
        value = t->GetSlot(slot_d->tuple_offset());
      }
    } else {
      value = node_->grouping_values_cache_[i];
    }
    // don't ignore NULLs; we want (1, NULL) to return a different hash
    // value than (NULL, 1)
    size_t hash_value =
        (value == NULL ? 0 : RawValue::GetHashValue(value, slot_d->type()));
    hash_combine(seed, hash_value);
  }
  return seed;
}

void AggregationNode::GroupingExprEquals::Init(
    TupleDescriptor* agg_tuple_d, const vector<Expr*>& grouping_exprs) {
  agg_tuple_desc_ = agg_tuple_d;
  grouping_exprs_ = &grouping_exprs;
}

bool AggregationNode::GroupingExprEquals::operator()( 
    Tuple* const& t1, Tuple* const& t2) const {
  for (int i = 0; i < grouping_exprs_->size(); ++i) {
    SlotDescriptor* slot_d = agg_tuple_desc_->slots()[i];
    const void* value1 = NULL;
    const void* value2 = NULL;

    if (t1 == NULL) {
      value1 = node_->grouping_values_cache_[i];
    } else if (!t1->IsNull(slot_d->null_indicator_offset())) {
      value1 = t1->GetSlot(slot_d->tuple_offset());
    }
    if (!t2->IsNull(slot_d->null_indicator_offset())) {
      value2 = t2->GetSlot(slot_d->tuple_offset());
    }
    
    if (value1 == NULL || value2 == NULL) {
      // nulls are considered equal for the purpose of grouping
      if (value1 != NULL || value2 != NULL) return false;
    } else {
      if (!RawValue::Eq(value1, value2, slot_d->type())) return false;
    }
  }
  return true;
} 

Status AggregationNode::Prepare(RuntimeState* state) {
  RETURN_IF_ERROR(ExecNode::Prepare(state));

  agg_tuple_desc_ = state->desc_tbl().GetTupleDescriptor(agg_tuple_id_);
  RETURN_IF_ERROR(Expr::Prepare(grouping_exprs_, state, child(0)->row_desc()));
  RETURN_IF_ERROR(Expr::Prepare(aggregate_exprs_, state, child(0)->row_desc()));
  hash_fn_.Init(agg_tuple_desc_, grouping_exprs_);
  equals_fn_.Init(agg_tuple_desc_, grouping_exprs_);
  // TODO: how many buckets?
  hash_tbl_.reset(new HashTable(5, hash_fn_, equals_fn_));
  
  // Determine the number of string slots in the output
  for (vector<Expr*>::const_iterator expr = aggregate_exprs_.begin();
       expr != aggregate_exprs_.end(); ++expr) {
    AggregateExpr* agg_expr = static_cast<AggregateExpr*>(*expr);
    if (agg_expr->type() == TYPE_STRING) ++num_string_slots_;
  }
  
  grouping_values_cache_.resize(grouping_exprs_.size());
  if (grouping_exprs_.empty()) {
    // create single output tuple now; we need to output something
    // even if our input is empty
    singleton_output_tuple_ = ConstructAggTuple();
  }
  return Status::OK;
}

Status AggregationNode::Open(RuntimeState* state) {
  COUNTER_SCOPED_TIMER(runtime_profile_->total_time_counter());

  RETURN_IF_ERROR(children_[0]->Open(state));

  RowBatch batch(children_[0]->row_desc(), state->batch_size());
  int64_t num_input_rows = 0;
  int64_t num_agg_rows = 0;
  while (true) {
    bool eos;
    RETURN_IF_ERROR(children_[0]->GetNext(state, &batch, &eos));

    if (VLOG_IS_ON(2)) {
      for (int i = 0; i < batch.num_rows(); ++i) {
        TupleRow* row = batch.GetRow(i);
        VLOG(2) << "input row: " << PrintRow(row, children_[0]->row_desc());
      }
    }

    if (singleton_output_tuple_ != NULL) {
      for (int i = 0; i < batch.num_rows(); ++i) {
        current_row_ = batch.GetRow(i);
        UpdateAggTuple(singleton_output_tuple_, current_row_);
      }
    } else {
      for (int i = 0; i < batch.num_rows(); ++i) {
        current_row_ = batch.GetRow(i);
        // Compute and cache the grouping exprs for current_row_
        ComputeGroupingValues();
        AggregationTuple* agg_tuple = NULL; 
        // find(NULL) finds the entry for current_row_
        HashTable::iterator entry = hash_tbl_->find(NULL);
        if (entry == hash_tbl_->end()) {
          // new entry
          agg_tuple = ConstructAggTuple();
          hash_tbl_->insert(agg_tuple->tuple());
          ++num_agg_rows;
        } else {
          agg_tuple = reinterpret_cast<AggregationTuple*>(*entry);
        }
        UpdateAggTuple(agg_tuple, current_row_);
      }
    }
    num_input_rows += batch.num_rows();
    if (eos) break;
    batch.Reset();
  }
  RETURN_IF_ERROR(children_[0]->Close(state));
  if (singleton_output_tuple_ != NULL) {
    hash_tbl_->insert(singleton_output_tuple_->tuple());
    ++num_agg_rows;
  }
  VLOG(1) << "aggregated " << num_input_rows << " input rows into "
          << num_agg_rows << " output rows";
  output_iterator_ = hash_tbl_->begin();
  return Status::OK;
}

Status AggregationNode::GetNext(RuntimeState* state, RowBatch* row_batch, bool* eos) {
  COUNTER_SCOPED_TIMER(runtime_profile_->total_time_counter());
  if (ReachedLimit()) {
    *eos = true;
    return Status::OK;
  }
  while (output_iterator_ != hash_tbl_->end() && !row_batch->IsFull()) {
    int row_idx = row_batch->AddRow();
    TupleRow* row = row_batch->GetRow(row_idx);
    row->SetTuple(0, *output_iterator_);
    if (ExecNode::EvalConjuncts(row)) {
      VLOG(1) << "output row: " << PrintRow(row, row_desc());
      row_batch->CommitLastRow();
      ++num_rows_returned_;
      if (ReachedLimit()) {
        *eos = true;
        return Status::OK;
      }
    }
    ++output_iterator_;
  }
  *eos = output_iterator_ == hash_tbl_->end();
  return Status::OK;
}

Status AggregationNode::Close(RuntimeState* state) {
  RETURN_IF_ERROR(ExecNode::Close(state));
  return Status::OK;
}

void AggregationNode::ComputeGroupingValues() {
  DCHECK(current_row_ != NULL);
  for (int i = 0; i < grouping_exprs_.size(); ++i) {
    grouping_values_cache_[i] = grouping_exprs_[i]->GetValue(current_row_);
  }
}

AggregationTuple* AggregationNode::ConstructAggTuple() {
  AggregationTuple* agg_out_tuple = 
      AggregationTuple::Create(agg_tuple_desc_->byte_size(), num_string_slots_, tuple_pool_.get());
  Tuple* agg_tuple = agg_out_tuple->tuple();

  vector<SlotDescriptor*>::const_iterator slot_d = agg_tuple_desc_->slots().begin();
  // copy grouping values
  for (int i = 0; i < grouping_exprs_.size(); ++i, ++slot_d) {
    void* grouping_val = grouping_values_cache_[i];
    if (grouping_val == NULL) {
      agg_tuple->SetNull((*slot_d)->null_indicator_offset());
    } else {
      RawValue::Write(grouping_val, agg_tuple, *slot_d, tuple_pool_.get());
    }
  }

  // All aggregate values except for COUNT start out with NULL
  // (so that SUM(<col>) stays NULL if <col> only contains NULL values).
  for (int i = 0; i < aggregate_exprs_.size(); ++i, ++slot_d) {
    AggregateExpr* agg_expr = static_cast<AggregateExpr*>(aggregate_exprs_[i]);
    if ((*slot_d)->is_nullable()) {
      DCHECK_NE(agg_expr->agg_op(), TAggregationOp::COUNT);
      agg_tuple->SetNull((*slot_d)->null_indicator_offset());
    } else {
      // For distributed plans, some SUMs (distributed count(*) will be non-nullable)
      DCHECK(agg_expr->agg_op() == TAggregationOp::COUNT ||
             agg_expr->agg_op() == TAggregationOp::SUM);
      // we're only aggregating into bigint slots and never return NULL
      *reinterpret_cast<int64_t*>(agg_tuple->GetSlot((*slot_d)->tuple_offset())) = 0;
    }
  }

  return agg_out_tuple;
}

char* AggregationNode::AllocateStringBuffer(int new_size, int* allocated_size) {
  new_size = ::max(new_size, FreeList::MinSize());
  char* buffer = reinterpret_cast<char*>(
      string_buffer_free_list_.Allocate(new_size, allocated_size));
  if (buffer == NULL)  {
    buffer = reinterpret_cast<char*>(tuple_pool_->Allocate(new_size));
    *allocated_size = new_size;
  }
  return buffer;
}

inline void AggregationNode::UpdateStringSlot(AggregationTuple* tuple, int string_slot_idx,
                                       StringValue* dst, const StringValue* src) {
  int32_t* string_buffer_lengths = tuple->BufferLengths(agg_tuple_desc_->byte_size());
  int curr_size = string_buffer_lengths[string_slot_idx];
  if (curr_size < src->len) {
    string_buffer_free_list_.Add(reinterpret_cast<uint8_t*>(dst->ptr), curr_size);
    dst->ptr = AllocateStringBuffer(src->len, &(string_buffer_lengths[string_slot_idx]));
  }
  strncpy(dst->ptr, src->ptr, src->len);
  dst->len = src->len;
}

inline void AggregationNode::UpdateMinStringSlot(AggregationTuple* agg_tuple, 
                                          const NullIndicatorOffset& null_indicator_offset, 
                                          int string_slot_idx, void* slot, void* value) {
  DCHECK(value != NULL);
  Tuple* tuple = agg_tuple->tuple();
  StringValue* dst_value = static_cast<StringValue*>(slot);
  StringValue* src_value = static_cast<StringValue*>(value);

  if (tuple->IsNull(null_indicator_offset)) {
    tuple->SetNotNull(null_indicator_offset);
  } else if (src_value->Compare(*dst_value) >= 0) {
    return;
  }
  UpdateStringSlot(agg_tuple, string_slot_idx, dst_value, src_value);
}

inline void AggregationNode::UpdateMaxStringSlot(AggregationTuple* agg_tuple, 
                                          const NullIndicatorOffset& null_indicator_offset,
                                          int string_slot_idx, void* slot, void* value) {
  DCHECK(value != NULL);
  Tuple* tuple = agg_tuple->tuple();
  StringValue* dst_value = static_cast<StringValue*>(slot);
  StringValue* src_value = static_cast<StringValue*>(value);

  if (tuple->IsNull(null_indicator_offset)) {
    tuple->SetNotNull(null_indicator_offset);
  } else if (src_value->Compare(*dst_value) <= 0) {
    return;
  }
  UpdateStringSlot(agg_tuple, string_slot_idx, dst_value, src_value);
}

template <typename T>
void UpdateMinSlot(Tuple* tuple, const NullIndicatorOffset& null_indicator_offset,
                   void* slot, void* value) {
  DCHECK(value != NULL);
  T* t_slot = static_cast<T*>(slot);
  if (tuple->IsNull(null_indicator_offset)) {
    tuple->SetNotNull(null_indicator_offset);
    *t_slot = *static_cast<T*>(value);
  } else {
    *t_slot = min(*t_slot, *static_cast<T*>(value));
  }
}

template <typename T>
void UpdateMaxSlot(Tuple* tuple, const NullIndicatorOffset& null_indicator_offset,
                   void* slot, void* value) {
  DCHECK(value != NULL);
  T* t_slot = static_cast<T*>(slot);
  if (tuple->IsNull(null_indicator_offset)) {
    tuple->SetNotNull(null_indicator_offset);
    *t_slot = *static_cast<T*>(value);
  } else {
    *t_slot = max(*t_slot, *static_cast<T*>(value));
  }
}

template <typename T>
void UpdateSumSlot(Tuple* tuple, const NullIndicatorOffset& null_indicator_offset,
                   void* slot, void* value) {
  DCHECK(value != NULL);
  T* t_slot = static_cast<T*>(slot);
  if (tuple->IsNull(null_indicator_offset)) {
    tuple->SetNotNull(null_indicator_offset);
    *t_slot = *static_cast<T*>(value);
  } else {
    *t_slot += *static_cast<T*>(value);
  }
}

void AggregationNode::UpdateAggTuple(AggregationTuple* agg_out_tuple, TupleRow* row) {
  Tuple* tuple = agg_out_tuple->tuple();
  int string_slot_idx = -1;
  vector<SlotDescriptor*>::const_iterator slot_d =
      agg_tuple_desc_->slots().begin() + grouping_exprs_.size();
  for (vector<Expr*>::iterator expr = aggregate_exprs_.begin();
       expr != aggregate_exprs_.end(); ++expr, ++slot_d) {
    void* slot = tuple->GetSlot((*slot_d)->tuple_offset());
    AggregateExpr* agg_expr = static_cast<AggregateExpr*>(*expr);

    // keep track of which string slot we are on
    if (agg_expr->type() == TYPE_STRING) {
      ++string_slot_idx;
    }

    // deal with COUNT(*) separately (no need to check the actual child expr value)
    if (agg_expr->agg_op() == TAggregationOp::COUNT && agg_expr->is_star()) {
      // we're only aggregating into bigint slots
      DCHECK_EQ((*slot_d)->type(), TYPE_BIGINT);
      ++*reinterpret_cast<int64_t*>(slot);
      continue;
    }

    // determine value of aggregate's child expr
    void* value = agg_expr->GetChild(0)->GetValue(row);
    if (value == NULL) {
      // NULLs don't get aggregated
      continue;
    }

    switch (agg_expr->agg_op()) {
      case TAggregationOp::COUNT:
        ++*reinterpret_cast<int64_t*>(slot);
        break;

      case TAggregationOp::MIN:
        switch (agg_expr->type()) {
          case TYPE_BOOLEAN:
            UpdateMinSlot<bool>(tuple,
                                (*slot_d)->null_indicator_offset(), slot, value);
            break;
          case TYPE_TINYINT:
            UpdateMinSlot<int8_t>(tuple,
                                  (*slot_d)->null_indicator_offset(), slot, value);
            break;
          case TYPE_SMALLINT:
            UpdateMinSlot<int16_t>(tuple,
                                   (*slot_d)->null_indicator_offset(), slot, value);
            break;
          case TYPE_INT:
            UpdateMinSlot<int32_t>(tuple,
                                   (*slot_d)->null_indicator_offset(), slot, value);
            break;
          case TYPE_BIGINT:
            UpdateMinSlot<int64_t>(tuple,
                                   (*slot_d)->null_indicator_offset(), slot, value);
            break;
          case TYPE_FLOAT:
            UpdateMinSlot<float>(tuple, (*slot_d)->null_indicator_offset(), slot, value);
            break;
          case TYPE_DOUBLE:
            UpdateMinSlot<double>(tuple, (*slot_d)->null_indicator_offset(), slot, value);
            break;
          case TYPE_TIMESTAMP:
            UpdateMinSlot<TimestampValue>(tuple,
                                          (*slot_d)->null_indicator_offset(),
                                          slot, value);
            break;
          case TYPE_STRING:
            UpdateMinStringSlot(agg_out_tuple, (*slot_d)->null_indicator_offset(), 
                                string_slot_idx, slot, value);
            break;
          default:
            DCHECK(false) << "invalid type: " << TypeToString(agg_expr->type());
        };
        break;

      case TAggregationOp::MAX:
        switch (agg_expr->type()) {
          case TYPE_BOOLEAN:
            UpdateMaxSlot<bool>(tuple, (*slot_d)->null_indicator_offset(), slot, value);
            break;
          case TYPE_TINYINT:
            UpdateMaxSlot<int8_t>(tuple, (*slot_d)->null_indicator_offset(), slot, value);
            break;
          case TYPE_SMALLINT:
            UpdateMaxSlot<int16_t>(tuple,
                                   (*slot_d)->null_indicator_offset(), slot, value);
            break;
          case TYPE_INT:
            UpdateMaxSlot<int32_t>(tuple,
                                   (*slot_d)->null_indicator_offset(), slot, value);
            break;
          case TYPE_BIGINT:
            UpdateMaxSlot<int64_t>(tuple,
                                   (*slot_d)->null_indicator_offset(), slot, value);
            break;
          case TYPE_FLOAT:
            UpdateMaxSlot<float>(tuple, (*slot_d)->null_indicator_offset(), slot, value);
            break;
          case TYPE_DOUBLE:
            UpdateMaxSlot<double>(tuple, (*slot_d)->null_indicator_offset(), slot, value);
            break;
          case TYPE_TIMESTAMP:
            UpdateMaxSlot<TimestampValue>(tuple,
                                          (*slot_d)->null_indicator_offset(),
                                          slot, value);
            break;
          case TYPE_STRING:
            UpdateMaxStringSlot(agg_out_tuple, (*slot_d)->null_indicator_offset(), 
                                string_slot_idx, slot, value);
            break;
          default:
            DCHECK(false) << "invalid type: " << TypeToString(agg_expr->type());
        };
        break;

      case TAggregationOp::SUM:
        switch (agg_expr->type()) {
          case TYPE_BIGINT:
            UpdateSumSlot<int64_t>(tuple,
                                   (*slot_d)->null_indicator_offset(), slot, value);
            break;
          case TYPE_DOUBLE:
            UpdateSumSlot<double>(tuple, (*slot_d)->null_indicator_offset(), slot, value);
            break;
          default:
            DCHECK(false) << "invalid type: " << TypeToString(agg_expr->type());
        };
        break;

      default:
        DCHECK(false) << "bad aggregate operator: " << agg_expr->agg_op();
    }
  }
}

void AggregationNode::DebugString(int indentation_level, stringstream* out) const {
  *out << string(indentation_level * 2, ' ');
  *out << "AggregationNode(tuple_id=" << agg_tuple_id_
       << " grouping_exprs=" << Expr::DebugString(grouping_exprs_)
       << " agg_exprs=" << Expr::DebugString(aggregate_exprs_);
  ExecNode::DebugString(indentation_level, out);
  *out << ")";
}
