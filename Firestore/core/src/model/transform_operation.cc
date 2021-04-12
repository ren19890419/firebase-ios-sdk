/*
 * Copyright 2019 Google
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "Firestore/core/src/model/transform_operation.h"

#include <memory>
#include <ostream>
#include <utility>
#include <vector>

#include "Firestore/core/include/firebase/firestore/timestamp.h"
#include "Firestore/core/src/util/hard_assert.h"
#include "Firestore/core/src/util/to_string.h"
#include "Firestore/core/src/model/server_timestamp_util.h"
#include "absl/algorithm/container.h"
#include "absl/strings/str_cat.h"

namespace firebase {
namespace firestore {
namespace model {

using Type = TransformOperation::Type;

// MARK: - TransformOperation

TransformOperation::TransformOperation(std::shared_ptr<const Rep> rep)
    : rep_(std::move(rep)) {
}

/** Returns whether the two are equal. */
bool operator==(const TransformOperation& lhs, const TransformOperation& rhs) {
  return lhs.rep_ == nullptr
             ? rhs.rep_ == nullptr
             : (rhs.rep_ != nullptr && lhs.rep_->Equals(*rhs.rep_));
}

std::ostream& operator<<(std::ostream& os, const TransformOperation& op) {
  return os << op.ToString();
}

// MARK: - ServerTimestampTransform

static_assert(sizeof(TransformOperation) == sizeof(ServerTimestampTransform),
              "No additional members allowed (everything must go in Rep)");

class ServerTimestampTransform::Rep : public TransformOperation::Rep {
 public:
  Type type() const override {
    return Type::ServerTimestamp;
  }

  google_firestore_v1_Value ApplyToLocalView(
      const absl::optional<google_firestore_v1_Value>& previous_value,
      const Timestamp& local_write_time) const override {
    return google_firestore_v1_Value::FromServerTimestamp(local_write_time, previous_value);
  }

  google_firestore_v1_Value ApplyToRemoteDocument(
      const absl::optional<google_firestore_v1_Value>&,
      const google_firestore_v1_Value& transform_result) const override {
    return transform_result;
  }

  absl::optional<google_firestore_v1_Value> ComputeBaseValue(
      const absl::optional<google_firestore_v1_Value>&) const override {
    // Server timestamps are idempotent and don't require a base value.
    return absl::nullopt;
  }

  bool Equals(const TransformOperation::Rep& other) const override {
    // All ServerTimestampTransform objects are equal.
    return other.type() == Type::ServerTimestamp;
  }

  size_t Hash() const override {
    // An arbitrary number, since all instances are equal.
    return 37;
  }

  std::string ToString() const override {
    return "ServerTimestamp";
  }
};

ServerTimestampTransform::ServerTimestampTransform()
    : TransformOperation(std::make_shared<const Rep>()) {
}

// MARK: - ArrayTransform

static_assert(sizeof(TransformOperation) == sizeof(ArrayTransform),
              "No additional members allowed (everything must go in Rep)");

/**
 * Transforms an array via a union or remove operation (for convenience, we use
 * this class for both Type::ArrayUnion and Type::ArrayRemove).
 */
class ArrayTransform::Rep : public TransformOperation::Rep {
 public:
  Rep(Type type, std::vector<google_firestore_v1_Value> elements)
      : type_(type), elements_(std::move(elements)) {
  }

  Type type() const override {
    return type_;
  }

  google_firestore_v1_Value ApplyToLocalView(
      const absl::optional<google_firestore_v1_Value>& previous_value,
      const Timestamp&) const override {
    return Apply(previous_value);
  }

  google_firestore_v1_Value ApplyToRemoteDocument(
      const absl::optional<google_firestore_v1_Value>& previous_value,
      const google_firestore_v1_Value&) const override {
    // The server just sends null as the transform result for array operations,
    // so we have to calculate a result the same as we do for local
    // applications.
    return Apply(previous_value);
  }

  absl::optional<google_firestore_v1_Value> ComputeBaseValue(
      const absl::optional<google_firestore_v1_Value>&) const override {
    // Array transforms are idempotent and don't require a base value.
    return absl::nullopt;
  }

  const std::vector<google_firestore_v1_Value>& elements() const {
    return elements_;
  }

  bool Equals(const TransformOperation::Rep& other) const override;

  size_t Hash() const override;

  std::string ToString() const override;

  static const std::vector<google_firestore_v1_Value>& Elements(
      const TransformOperation& op);

 private:
  friend class ArrayTransform;

  /**
   * Inspects the provided value, returning a mutable copy of the internal array
   * if it's of type Array and an empty mutable array if it's nil or any other
   * type of google_firestore_v1_Value.
   */
  static std::vector<google_firestore_v1_Value> Coercedgoogle_firestore_v1_ValuesArray(
      const absl::optional<google_firestore_v1_Value>& value);

  google_firestore_v1_Value Apply(
      const absl::optional<google_firestore_v1_Value>& previous_value) const;

  Type type_;
  std::vector<google_firestore_v1_Value> elements_;
};

namespace {

constexpr bool IsArrayTransform(Type type) {
  return type == Type::ArrayUnion || type == Type::ArrayRemove;
}

}  // namespace

ArrayTransform::ArrayTransform(Type type,
                               std::vector<google_firestore_v1_Value> elements)
    : TransformOperation(
          std::make_shared<const Rep>(type, std::move(elements))) {
  HARD_ASSERT(IsArrayTransform(type), "Expected array transform type; got %s",
              type);
}

ArrayTransform::ArrayTransform(const TransformOperation& op)
    : TransformOperation(op) {
  HARD_ASSERT(IsArrayTransform(op.type()),
              "Expected array transform type; got %s", op.type());
}

const std::vector<google_firestore_v1_Value>& ArrayTransform::elements() const {
  return array_rep().elements_;
}

const ArrayTransform::Rep& ArrayTransform::array_rep() const {
  return static_cast<const ArrayTransform::Rep&>(rep());
}

bool ArrayTransform::Rep::Equals(const TransformOperation::Rep& other) const {
  if (other.type() != type()) {
    return false;
  }
  auto other_rep = static_cast<const ArrayTransform::Rep&>(other);
  if (other_rep.elements_.size() != elements_.size()) {
    return false;
  }
  for (size_t i = 0; i < elements_.size(); i++) {
    if (other_rep.elements_[i] != elements_[i]) {
      return false;
    }
  }
  return true;
}

size_t ArrayTransform::Rep::Hash() const {
  size_t result = 37;
  result = 31 * result + (type() == Type::ArrayUnion ? 1231 : 1237);
  for (const google_firestore_v1_Value& element : elements_) {
    result = 31 * result + element.Hash();
  }
  return result;
}

std::string ArrayTransform::Rep::ToString() const {
  const char* name = type_ == Type::ArrayUnion ? "ArrayUnion" : "ArrayRemove";
  return absl::StrCat(name, "(", util::ToString(elements_), ")");
}

google_firestore_v1_Value::Array ArrayTransform::Rep::Coercedgoogle_firestore_v1_ValuesArray(
    const absl::optional<google_firestore_v1_Value>& value) {
  if (value && value->type() == google_firestore_v1_Value::Type::Array) {
    return value->array_value();
  } else {
    // coerce to empty array.
    return {};
  }
}

google_firestore_v1_Value ArrayTransform::Rep::Apply(
    const absl::optional<google_firestore_v1_Value>& previous_value) const {
  google_firestore_v1_Value::Array result = Coercedgoogle_firestore_v1_ValuesArray(previous_value);
  for (const google_firestore_v1_Value& element : elements_) {
    auto pos = absl::c_find(result, element);
    if (type_ == Type::ArrayUnion) {
      if (pos == result.end()) {
        result.push_back(element);
      }
    } else {
      HARD_ASSERT(type_ == Type::ArrayRemove);
      for (size_t i = 0; i < result.size();) {
        if (element == result.at(i)) {
          result.erase(result.cbegin() + i);
        } else {
          ++i;
        }
      }
    }
  }
  return google_firestore_v1_Value::FromArray(std::move(result));
}

// MARK: - NumericIncrementTransform

static_assert(sizeof(TransformOperation) == sizeof(NumericIncrementTransform),
              "No additional members allowed (everything must go in Rep)");

class NumericIncrementTransform::Rep : public TransformOperation::Rep {
 public:
  explicit Rep(google_firestore_v1_Value operand)
      : operand_(std::move(operand)) {
  }

  Type type() const override {
    return Type::Increment;
  }

  google_firestore_v1_Value ApplyToLocalView(
      const absl::optional<google_firestore_v1_Value>& previous_value,
      const Timestamp& local_write_time) const override;

  google_firestore_v1_Value ApplyToRemoteDocument(
      const absl::optional<google_firestore_v1_Value>&,
      const google_firestore_v1_Value& transform_result) const override {
    return transform_result;
  }

  absl::optional<google_firestore_v1_Value> ComputeBaseValue(
      const absl::optional<google_firestore_v1_Value>& previous_value)
      const override;

  google_firestore_v1_Value operand() const {
    return operand_;
  }

  bool Equals(const TransformOperation::Rep& other) const override;

  size_t Hash() const override {
    return operand_.Hash();
  }

  std::string ToString() const override {
    return absl::StrCat("NumericIncrement(", operand_.ToString(), ")");
  }

 private:
  friend class NumericIncrementTransform;

  google_firestore_v1_Value operand_;
};

NumericIncrementTransform::NumericIncrementTransform(google_firestore_v1_Value operand)
    : TransformOperation(std::make_shared<Rep>(operand)) {
  HARD_ASSERT(operand.is_number());
}

NumericIncrementTransform::NumericIncrementTransform(
    const TransformOperation& op)
    : TransformOperation(op) {
  HARD_ASSERT(op.type() == Type::Increment, "Expected increment type; got %s",
              op.type());
}

const google_firestore_v1_Value& NumericIncrementTransform::operand() const {
  return static_cast<const Rep&>(rep()).operand_;
}

namespace {

/**
 * Implements saturating integer addition. Overflows are resolved to
 * LONG_MAX/LONG_MIN.
 */
int64_t SafeIncrement(int64_t x, int64_t y) {
  if (x > 0 && y > LONG_MAX - x) {
    return LONG_MAX;
  }

  if (x < 0 && y < LONG_MIN - x) {
    return LONG_MIN;
  }

  return x + y;
}

double AsDouble(const google_firestore_v1_Value& value) {
  if (value.type() == google_firestore_v1_Value::Type::Double) {
    return value.double_value();
  } else if (value.type() == google_firestore_v1_Value::Type::Integer) {
    return static_cast<double>(value.integer_value());
  } else {
    HARD_FAIL("Expected 'operand' to be of numeric type, but was %s (type %s)",
              value.ToString(), value.type());
  }
}

}  // namespace

google_firestore_v1_Value NumericIncrementTransform::Rep::ApplyToLocalView(
    const absl::optional<google_firestore_v1_Value>& previous_value,
    const Timestamp& /* local_write_time */) const {
  absl::optional<google_firestore_v1_Value> base_value = ComputeBaseValue(previous_value);

  // Return an integer value only if the previous value and the operand is an
  // integer.
  if (base_value && base_value->type() == google_firestore_v1_Value::Type::Integer &&
      operand_.type() == google_firestore_v1_Value::Type::Integer) {
    int64_t sum =
        SafeIncrement(base_value->integer_value(), operand_.integer_value());
    return google_firestore_v1_Value::FromInteger(sum);
  } else {
    HARD_ASSERT(base_value && base_value->is_number(),
                "'base_value' is not of numeric type");
    double sum = AsDouble(*base_value) + AsDouble(operand_);
    return google_firestore_v1_Value::FromDouble(sum);
  }
}

absl::optional<google_firestore_v1_Value> NumericIncrementTransform::Rep::ComputeBaseValue(
    const absl::optional<google_firestore_v1_Value>& previous_value) const {
  return previous_value && previous_value->is_number()
             ? previous_value
             : absl::optional<google_firestore_v1_Value>{google_firestore_v1_Value::FromInteger(0)};
}

bool NumericIncrementTransform::Rep::Equals(
    const TransformOperation::Rep& other) const {
  if (other.type() != type()) {
    return false;
  }

  auto other_rep = static_cast<const NumericIncrementTransform::Rep&>(other);
  return operand_ == other_rep.operand_;
}

}  // namespace model
}  // namespace firestore
}  // namespace firebase
