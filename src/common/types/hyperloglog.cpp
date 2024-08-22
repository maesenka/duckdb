#include "duckdb/common/types/hyperloglog.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "hyperloglog.hpp"

#include <math.h>

namespace duckdb_hll {
struct robj; // NOLINT
}

namespace duckdb {

idx_t HyperLogLog::Count() const {
	uint32_t c[Q + 2] = {0};
	ExtractCounts(c);
	return static_cast<idx_t>(EstimateCardinality(c));
}

//! Algorithm 2
void HyperLogLog::Merge(const HyperLogLog &other) {
	for (idx_t i = 0; i < M; ++i) {
		Update(i, other.k[i]);
	}
}

//! Algorithm 4
void HyperLogLog::ExtractCounts(uint32_t *c) const {
	for (idx_t i = 0; i < M; ++i) {
		c[k[i]]++;
	}
}

//! Taken from redis code
static double HLLSigma(double x) {
	if (x == 1.) {
		return std::numeric_limits<double>::infinity();
	}
	double z_prime;
	double y = 1;
	double z = x;
	do {
		x *= x;
		z_prime = z;
		z += x * y;
		y += y;
	} while (z_prime != z);
	return z;
}

//! Taken from redis code
static double HLLTau(double x) {
	if (x == 0. || x == 1.) {
		return 0.;
	}
	double z_prime;
	double y = 1.0;
	double z = 1 - x;
	do {
		x = sqrt(x);
		z_prime = z;
		y *= 0.5;
		z -= pow(1 - x, 2) * y;
	} while (z_prime != z);
	return z / 3;
}

//! Algorithm 6
int64_t HyperLogLog::EstimateCardinality(uint32_t *c) {
	auto z = M * HLLTau((double(M) - c[Q]) / double(M));

	for (idx_t k = Q; k >= 1; --k) {
		z += c[k];
		z *= 0.5;
	}

	z += M * HLLSigma(c[0] / double(M));

	return llroundl(ALPHA * M * M / z);
}

void HyperLogLog::Update(Vector &input, Vector &hash_vec, const idx_t count) {
	UnifiedVectorFormat idata;
	input.ToUnifiedFormat(count, idata);

	UnifiedVectorFormat hdata;
	hash_vec.ToUnifiedFormat(count, hdata);
	const auto hashes = UnifiedVectorFormat::GetData<hash_t>(hdata);

	if (hash_vec.GetVectorType() == VectorType::CONSTANT_VECTOR) {
		if (idata.validity.RowIsValid(0)) {
			InsertElement(hashes[0]);
		}
	} else {
		for (idx_t i = 0; i < count; ++i) {
			if (idata.validity.RowIsValid(idata.sel->get_index(i))) {
				const auto hash = hashes[hdata.sel->get_index(i)];
				InsertElement(hash);
			}
		}
	}
}

unique_ptr<HyperLogLog> HyperLogLog::Copy() const {
	auto result = make_uniq<HyperLogLog>();
	memcpy(result->k, this->k, sizeof(k));
	D_ASSERT(result->Count() == Count());
	return result;
}

class HLLV1 {
public:
	HLLV1() {
		hll = duckdb_hll::hll_create();
		duckdb_hll::hllSparseToDense(hll);
	}

	~HLLV1() {
		duckdb_hll::hll_destroy(hll);
	}

public:
	static idx_t GetSize() {
		return duckdb_hll::get_size();
	}

	data_ptr_t GetPtr() const {
		return data_ptr_cast((hll)->ptr);
	}

	void ToNew(HyperLogLog &new_hll) const {
		const idx_t mult = duckdb_hll::num_registers() / HyperLogLog::M;
		// Old implementation used more registers, so we compress the registers, losing some accuracy
		for (idx_t i = 0; i < HyperLogLog::M; i++) {
			uint8_t max_old = 0;
			for (idx_t j = 0; j < mult; j++) {
				D_ASSERT(i * mult + j < duckdb_hll::num_registers());
				max_old = MaxValue<uint8_t>(max_old, duckdb_hll::get_register(hll, i * mult + j));
			}
			new_hll.Update(i, max_old);
		}
		D_ASSERT(IsWithinAcceptableRange(new_hll.Count(), Count()));
	}

	void FromNew(const HyperLogLog &new_hll) {
		const auto new_hll_count = new_hll.Count();
		if (new_hll_count == 0) {
			return;
		}

		const idx_t mult = duckdb_hll::num_registers() / HyperLogLog::M;
		// When going from less to more registers, we cannot just duplicate the registers,
		// as each register in the new HLL is the minimum of 'mult' registers in the old HLL.
		// Duplicating will make for VERY large over-estimations. Instead, we do the following:

		// Set the first of every 'mult' registers in the old HLL to the value in the new HLL
		// This ensures that we can convert OLD to NEW without loss of information
		idx_t sum = 0;
		for (idx_t i = 0; i < HyperLogLog::M; i++) {
			const auto max_new = MinValue(new_hll.GetRegister(i), duckdb_hll::maximum_zeros());
			duckdb_hll::set_register(hll, i * mult, max_new);
			sum += max_new;
		}
		const uint8_t avg = NumericCast<uint8_t>(sum / HyperLogLog::M);

		// Set all other registers to a default value, starting with the avg, which is optimized within 4 iterations
		uint8_t default_val = avg;
		for (uint8_t epsilon = 4; epsilon >= 1; epsilon--) {
			for (idx_t i = 0; i < HyperLogLog::M; i++) {
				const auto max_new = MinValue(new_hll.GetRegister(i), duckdb_hll::maximum_zeros());
				for (idx_t j = 1; j < mult; j++) {
					D_ASSERT(i * mult + j < duckdb_hll::num_registers());
					duckdb_hll::set_register(hll, i * mult + j, MinValue(max_new, default_val));
				}
			}
			if (IsWithinAcceptableRange(new_hll_count, Count())) {
				break;
			}
			if (Count() > new_hll.Count()) {
				default_val -= epsilon;
			} else {
				default_val += epsilon;
			}
		}
		D_ASSERT(IsWithinAcceptableRange(new_hll_count, Count()));
	}

private:
	idx_t Count() const {
		size_t result;
		if (duckdb_hll::hll_count(hll, &result) != HLL_C_OK) {
			throw InternalException("Could not count HLL?");
		}
		return result;
	}

	bool IsWithinAcceptableRange(const idx_t &new_hll_count, const idx_t &old_hll_count) const {
		const auto newd = static_cast<double>(new_hll_count);
		const auto oldd = static_cast<double>(old_hll_count);
		return MaxValue(newd, oldd) / MinValue(newd, oldd) < ACCEPTABLE_Q_ERROR;
	}

private:
	static constexpr double ACCEPTABLE_Q_ERROR = 2;
	duckdb_hll::robj *hll;
};

void HyperLogLog::Serialize(Serializer &serializer) const {
	if (serializer.ShouldSerialize(3)) {
		serializer.WriteProperty(100, "type", HLLStorageType::HLL_V2);
		serializer.WriteProperty(101, "data", k, sizeof(k));
	} else {
		auto old = make_uniq<HLLV1>();
		old->FromNew(*this);

		serializer.WriteProperty(100, "type", HLLStorageType::HLL_V1);
		serializer.WriteProperty(101, "data", old->GetPtr(), old->GetSize());
	}
}

unique_ptr<HyperLogLog> HyperLogLog::Deserialize(Deserializer &deserializer) {
	auto result = make_uniq<HyperLogLog>();
	auto storage_type = deserializer.ReadProperty<HLLStorageType>(100, "type");
	switch (storage_type) {
	case HLLStorageType::HLL_V1: {
		auto old = make_uniq<HLLV1>();
		deserializer.ReadProperty(101, "data", old->GetPtr(), old->GetSize());
		old->ToNew(*result);
		break;
	}
	case HLLStorageType::HLL_V2:
		deserializer.ReadProperty(101, "data", result->k, sizeof(k));
		break;
	default:
		throw SerializationException("Unknown HyperLogLog storage type!");
	}
	return result;
}

} // namespace duckdb
