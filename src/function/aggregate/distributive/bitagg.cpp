#include "duckdb/function/aggregate/distributive_functions.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/null_value.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/common/vector_operations/aggregate_executor.hpp"
#include "duckdb/common/types/bit.hpp"
#include "duckdb/storage/statistics/numeric_statistics.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/common/types/cast_helpers.hpp"

namespace duckdb {

template <class T>
struct BitState {
	bool is_set;
	T value;
};

template <class OP>
static AggregateFunction GetBitfieldUnaryAggregate(LogicalType type) {
	switch (type.id()) {
	case LogicalTypeId::TINYINT:
		return AggregateFunction::UnaryAggregate<BitState<uint8_t>, int8_t, int8_t, OP>(type, type);
	case LogicalTypeId::SMALLINT:
		return AggregateFunction::UnaryAggregate<BitState<uint16_t>, int16_t, int16_t, OP>(type, type);
	case LogicalTypeId::INTEGER:
		return AggregateFunction::UnaryAggregate<BitState<uint32_t>, int32_t, int32_t, OP>(type, type);
	case LogicalTypeId::BIGINT:
		return AggregateFunction::UnaryAggregate<BitState<uint64_t>, int64_t, int64_t, OP>(type, type);
	case LogicalTypeId::HUGEINT:
		return AggregateFunction::UnaryAggregate<BitState<hugeint_t>, hugeint_t, hugeint_t, OP>(type, type);
	case LogicalTypeId::UTINYINT:
		return AggregateFunction::UnaryAggregate<BitState<uint8_t>, uint8_t, uint8_t, OP>(type, type);
	case LogicalTypeId::USMALLINT:
		return AggregateFunction::UnaryAggregate<BitState<uint16_t>, uint16_t, uint16_t, OP>(type, type);
	case LogicalTypeId::UINTEGER:
		return AggregateFunction::UnaryAggregate<BitState<uint32_t>, uint32_t, uint32_t, OP>(type, type);
	case LogicalTypeId::UBIGINT:
		return AggregateFunction::UnaryAggregate<BitState<uint64_t>, uint64_t, uint64_t, OP>(type, type);
	default:
		throw InternalException("Unimplemented bitfield type for unary aggregate");
	}
}

struct BitwiseOperation {
	template <class STATE>
	static void Initialize(STATE *state) {
		//  If there are no matching rows, returns a null value.
		state->is_set = false;
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void Operation(STATE *state, AggregateInputData &, INPUT_TYPE *input, ValidityMask &mask, idx_t idx) {
		if (!state->is_set) {
			OP::template Assign(state, input[idx]);
			state->is_set = true;
		} else {
			OP::template Execute(state, input[idx]);
		}
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void ConstantOperation(STATE *state, AggregateInputData &aggr_input_data, INPUT_TYPE *input,
	                              ValidityMask &mask, idx_t count) {
		OP::template Operation<INPUT_TYPE, STATE, OP>(state, aggr_input_data, input, mask, 0);
	}

	template <class INPUT_TYPE, class STATE>
	static void Assign(STATE *state, INPUT_TYPE input) {
		state->value = input;
	}

	template <class STATE, class OP>
	static void Combine(const STATE &source, STATE *target, AggregateInputData &) {
		if (!source.is_set) {
			// source is NULL, nothing to do.
			return;
		}
		if (!target->is_set) {
			// target is NULL, use source value directly.
			OP::template Assign(target, source.value);
			target->is_set = true;
		} else {
			OP::template Execute(target, source.value);
		}
	}

	template <class T, class STATE>
	static void Finalize(Vector &result, AggregateInputData &, STATE *state, T *target, ValidityMask &mask, idx_t idx) {
		if (!state->is_set) {
			mask.SetInvalid(idx);
		} else {
			target[idx] = state->value;
		}
	}

	static bool IgnoreNull() {
		return true;
	}
};

struct BitAndOperation : public BitwiseOperation {
	template <class INPUT_TYPE, class STATE>
	static void Execute(STATE *state, INPUT_TYPE input) {
		state->value &= input;
	}
};

struct BitOrOperation : public BitwiseOperation {
	template <class INPUT_TYPE, class STATE>
	static void Execute(STATE *state, INPUT_TYPE input) {
		state->value |= input;
	}
};

struct BitXorOperation : public BitwiseOperation {
	template <class INPUT_TYPE, class STATE>
	static void Execute(STATE *state, INPUT_TYPE input) {
		state->value ^= input;
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void ConstantOperation(STATE *state, AggregateInputData &aggr_input_data, INPUT_TYPE *input,
	                              ValidityMask &mask, idx_t count) {
		for (idx_t i = 0; i < count; i++) {
			Operation<INPUT_TYPE, STATE, OP>(state, aggr_input_data, input, mask, 0);
		}
	}
};

struct BitStringBitwiseOperation : public BitwiseOperation {
	template <class STATE>
	static void Destroy(STATE *state) {
		if (state->is_set && !state->value.IsInlined()) {
			delete[] state->value.GetDataUnsafe();
		}
	}

	template <class INPUT_TYPE, class STATE>
	static void Assign(STATE *state, INPUT_TYPE input) {
		D_ASSERT(state->is_set == false);
		if (input.IsInlined()) {
			state->value = input;
		} else { // non-inlined string, need to allocate space for it
			auto len = input.GetSize();
			auto ptr = new char[len];
			memcpy(ptr, input.GetDataUnsafe(), len);

			state->value = string_t(ptr, len);
		}
	}

	template <class T, class STATE>
	static void Finalize(Vector &result, AggregateInputData &, STATE *state, T *target, ValidityMask &mask, idx_t idx) {
		if (!state->is_set) {
			mask.SetInvalid(idx);
		} else {
			target[idx] = StringVector::AddStringOrBlob(result, state->value);
		}
	}
};

struct BitStringAndOperation : public BitStringBitwiseOperation {

	template <class INPUT_TYPE, class STATE>
	static void Execute(STATE *state, INPUT_TYPE input) {
		Bit::BitwiseAnd(input, state->value, state->value);
	}
};

struct BitStringOrOperation : public BitStringBitwiseOperation {

	template <class INPUT_TYPE, class STATE>
	static void Execute(STATE *state, INPUT_TYPE input) {
		Bit::BitwiseOr(input, state->value, state->value);
	}
};

struct BitStringXorOperation : public BitStringBitwiseOperation {
	template <class INPUT_TYPE, class STATE>
	static void Execute(STATE *state, INPUT_TYPE input) {
		Bit::BitwiseXor(input, state->value, state->value);
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void ConstantOperation(STATE *state, AggregateInputData &aggr_input_data, INPUT_TYPE *input,
	                              ValidityMask &mask, idx_t count) {
		for (idx_t i = 0; i < count; i++) {
			Operation<INPUT_TYPE, STATE, OP>(state, aggr_input_data, input, mask, 0);
		}
	}
};

void BitAndFun::RegisterFunction(BuiltinFunctions &set) {
	AggregateFunctionSet bit_and("bit_and");
	for (auto &type : LogicalType::Integral()) {
		bit_and.AddFunction(GetBitfieldUnaryAggregate<BitAndOperation>(type));
	}

	bit_and.AddFunction(
	    AggregateFunction::UnaryAggregateDestructor<BitState<string_t>, string_t, string_t, BitStringAndOperation>(
	        LogicalType::BIT, LogicalType::BIT));
	set.AddFunction(bit_and);
}

void BitOrFun::RegisterFunction(BuiltinFunctions &set) {
	AggregateFunctionSet bit_or("bit_or");
	for (auto &type : LogicalType::Integral()) {
		bit_or.AddFunction(GetBitfieldUnaryAggregate<BitOrOperation>(type));
	}
	bit_or.AddFunction(
	    AggregateFunction::UnaryAggregateDestructor<BitState<string_t>, string_t, string_t, BitStringOrOperation>(
	        LogicalType::BIT, LogicalType::BIT));
	set.AddFunction(bit_or);
}

void BitXorFun::RegisterFunction(BuiltinFunctions &set) {
	AggregateFunctionSet bit_xor("bit_xor");
	for (auto &type : LogicalType::Integral()) {
		bit_xor.AddFunction(GetBitfieldUnaryAggregate<BitXorOperation>(type));
	}
	bit_xor.AddFunction(
	    AggregateFunction::UnaryAggregateDestructor<BitState<string_t>, string_t, string_t, BitStringXorOperation>(
	        LogicalType::BIT, LogicalType::BIT));
	set.AddFunction(bit_xor);
}

template <class T, class INPUT_TYPE>
struct BitAggState {
	bool is_set;
	T value;
	INPUT_TYPE min;
	INPUT_TYPE max;
};

struct BitstringAggBindData : public FunctionData {
	Value min;
	Value max;

	unique_ptr<FunctionData> Copy() const override {
		return make_unique<BitstringAggBindData>(*this);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = (BitstringAggBindData &)other_p;
		if (min.IsNull() && other.min.IsNull() && max.IsNull() && other.max.IsNull()) {
			return true;
		}
		if (Value::NotDistinctFrom(min, other.min) && Value::NotDistinctFrom(max, other.max)) {
			return true;
		}
		return false;
	}
};

struct BitStringAggOperation {
	template <class STATE>
	static void Initialize(STATE *state) {
		state->is_set = false;
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void Operation(STATE *state, AggregateInputData &data, INPUT_TYPE *input, ValidityMask &mask, idx_t idx) {
		auto bind_agg_data = (BitstringAggBindData *)data.bind_data;
		if (!state->is_set) {
			state->min = bind_agg_data->min.GetValueUnsafe<INPUT_TYPE>();
			state->max = bind_agg_data->max.GetValueUnsafe<INPUT_TYPE>();
			idx_t bit_range = GetRange(bind_agg_data->min.GetValueUnsafe<INPUT_TYPE>(),
			                           bind_agg_data->max.GetValueUnsafe<INPUT_TYPE>());
			if (bit_range > 1000000000) { // for now capped at 1 billion bits
				throw OutOfRangeException("The range between min and max value is too large for bitstring aggregation");
			}
			idx_t len = bit_range % 8 ? (bit_range / 8) + 1 : bit_range / 8;
			len++;

			auto ptr = new char[len];
			auto target = string_t(ptr, len);
			Bit::SetEmptyBitString(target, bit_range);
			state->value = target;
			state->is_set = true;
		}
		if (input[idx] >= state->min && input[idx] <= state->max) {
			Execute(state, input[idx], bind_agg_data->min.GetValueUnsafe<INPUT_TYPE>());
		} else {
			throw OutOfRangeException("Value %s is outside of provided min and max range (%s <-> %s)",
			                          NumericHelper::ToString(input[idx]), NumericHelper::ToString(state->min),
			                          NumericHelper::ToString(state->max));
		}
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void ConstantOperation(STATE *state, AggregateInputData &aggr_input_data, INPUT_TYPE *input,
	                              ValidityMask &mask, idx_t count) {
		OP::template Operation<INPUT_TYPE, STATE, OP>(state, aggr_input_data, input, mask, 0);
	}

	template <class INPUT_TYPE>
	static idx_t GetRange(INPUT_TYPE min, INPUT_TYPE max) {
		return max - min + 1;
	}

	template <>
	idx_t GetRange(hugeint_t min, hugeint_t max) {
		idx_t val;
		if (Hugeint::TryCast(max - min + 1, val)) {
			return val;
		} else {
			throw OutOfRangeException("Range too large for bitstring aggregation");
		}
	}

	template <class INPUT_TYPE, class STATE>
	static void Execute(STATE *state, INPUT_TYPE input, INPUT_TYPE min) {
		Bit::SetBit(state->value, input - min, 1);
	}

	template <class STATE>
	static void Execute(STATE *state, hugeint_t input, hugeint_t min) {
		idx_t val;
		if (Hugeint::TryCast(input - min, val)) {
			Bit::SetBit(state->value, val, 1);
		} else {
			throw OutOfRangeException("Range too large for bitstring aggregation");
		}
	}

	template <class STATE, class OP>
	static void Combine(const STATE &source, STATE *target, AggregateInputData &) {
		if (!source.is_set) {
			return;
		}
		if (!target->is_set) {
			Assign(target, source.value);
			target->is_set = true;
		} else {
			Bit::BitwiseOr(source.value, target->value, target->value);
		}
	}

	template <class INPUT_TYPE, class STATE>
	static void Assign(STATE *state, INPUT_TYPE input) {
		D_ASSERT(state->is_set == false);
		if (input.IsInlined()) {
			state->value = input;
		} else { // non-inlined string, need to allocate space for it
			auto len = input.GetSize();
			auto ptr = new char[len];
			memcpy(ptr, input.GetDataUnsafe(), len);
			state->value = string_t(ptr, len);
		}
	}

	template <class T, class STATE>
	static void Finalize(Vector &result, AggregateInputData &, STATE *state, T *target, ValidityMask &mask, idx_t idx) {
		if (!state->is_set) {
			mask.SetInvalid(idx);
		} else {
			target[idx] = StringVector::AddStringOrBlob(result, state->value);
		}
	}

	template <class STATE>
	static void Destroy(STATE *state) {
		if (state->is_set && !state->value.IsInlined()) {
			delete[] state->value.GetDataUnsafe();
		}
	}

	static bool IgnoreNull() {
		return true;
	}
};

unique_ptr<BaseStatistics> BitstringPropagateStats(ClientContext &context, BoundAggregateExpression &expr,
                                                   FunctionData *bind_data,
                                                   vector<unique_ptr<BaseStatistics>> &child_stats,
                                                   NodeStatistics *node_stats) {

	if (child_stats[0] && node_stats && node_stats->has_max_cardinality) {
		auto &numeric_stats = (NumericStatistics &)*child_stats[0];
		if (numeric_stats.min.IsNull() || numeric_stats.max.IsNull()) {
			return nullptr;
		}
		auto bind_agg_data = (BitstringAggBindData *)bind_data;
		bind_agg_data->min = numeric_stats.min;
		bind_agg_data->max = numeric_stats.max;
	} else {
		throw BinderException("Could not retrieve required statistics. Alternatively, try by providing the statistics "
		                      "explicitly: BITSTRING_AGG(col, min, max) ");
	}
	return nullptr;
}

unique_ptr<FunctionData> BindBitstringAgg(ClientContext &context, AggregateFunction &function,
                                          vector<unique_ptr<Expression>> &arguments) {

	if (arguments.size() == 3) {
		auto bind_data = make_unique<BitstringAggBindData>();
		bind_data->min = ExpressionExecutor::EvaluateScalar(context, *arguments[1]);
		bind_data->max = ExpressionExecutor::EvaluateScalar(context, *arguments[2]);
		Function::EraseArgument(function, arguments, 2);
		Function::EraseArgument(function, arguments, 1);
		return bind_data;
	}
	return make_unique<BitstringAggBindData>();
}

template <class TYPE>
static void BindBitString(AggregateFunctionSet &bitstring_agg, const LogicalTypeId &type) {
	auto function =
	    AggregateFunction::UnaryAggregateDestructor<BitAggState<string_t, TYPE>, TYPE, string_t, BitStringAggOperation>(
	        type, LogicalType::BIT);
	function.bind = BindBitstringAgg;              // create new a 'BitstringAggBindData'
	function.statistics = BitstringPropagateStats; // stores min and max from column stats in BistringAggBindData
	bitstring_agg.AddFunction(function); // uses the BitstringAggBindData to access statistics for creating bitstring
	function.arguments = {type, type, type};
	function.statistics = nullptr; // min and max are provided as arguments
	bitstring_agg.AddFunction(function);
}

void BitStringAggFun::GetBitStringAggregate(const LogicalType &type, AggregateFunctionSet &bitstring_agg) {
	switch (type.id()) {
	case LogicalType::TINYINT: {
		return BindBitString<int8_t>(bitstring_agg, type.id());
	}
	case LogicalType::SMALLINT: {
		return BindBitString<int16_t>(bitstring_agg, type.id());
	}
	case LogicalType::INTEGER: {
		return BindBitString<int32_t>(bitstring_agg, type.id());
	}
	case LogicalType::BIGINT: {
		return BindBitString<int64_t>(bitstring_agg, type.id());
	}
	case LogicalType::HUGEINT: {
		return BindBitString<hugeint_t>(bitstring_agg, type.id());
	}
	case LogicalType::UTINYINT: {
		return BindBitString<uint8_t>(bitstring_agg, type.id());
	}
	case LogicalType::USMALLINT: {
		return BindBitString<uint16_t>(bitstring_agg, type.id());
	}
	case LogicalType::UINTEGER: {
		return BindBitString<uint32_t>(bitstring_agg, type.id());
	}
	case LogicalType::UBIGINT: {
		return BindBitString<uint64_t>(bitstring_agg, type.id());
	}
	default:
		throw InternalException("Unimplemented bitstring aggregate");
	}
}

void BitStringAggFun::RegisterFunction(BuiltinFunctions &set) {
	AggregateFunctionSet bitstring_agg("bitstring_agg");
	for (auto &type : LogicalType::Integral()) {
		GetBitStringAggregate(type, bitstring_agg);
	}
	set.AddFunction(bitstring_agg);
}

} // namespace duckdb
