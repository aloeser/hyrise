#include "abstract_histogram.hpp"

#include <cmath>

#include <algorithm>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "expression/evaluation/like_matcher.hpp"
#include "generic_histogram.hpp"
#include "histogram_utils.hpp"
#include "storage/create_iterable_from_segment.hpp"

#include "resolve_type.hpp"

namespace opossum {

using namespace opossum::histogram;  // NOLINT

template <typename T>
AbstractHistogram<T>::AbstractHistogram() : _supported_characters(""), _string_prefix_length(0ul) {}

template <>
AbstractHistogram<std::string>::AbstractHistogram() {
  const auto pair = get_default_or_check_string_histogram_prefix_settings();
  _supported_characters = pair.first;
  _string_prefix_length = pair.second;
}

template <>
AbstractHistogram<std::string>::AbstractHistogram(const std::string& supported_characters,
                                                  const size_t string_prefix_length)
    : _supported_characters(supported_characters), _string_prefix_length(string_prefix_length) {
  Assert(check_prefix_settings(_supported_characters, _string_prefix_length), "Invalid prefix settings.");
}

template <typename T>
std::string AbstractHistogram<T>::description() const {
  std::stringstream stream;
  stream << histogram_name() << std::endl;
  stream << "  distinct    " << total_distinct_count() << std::endl;
  stream << "  min         " << minimum() << std::endl;
  stream << "  max         " << maximum() << std::endl;
  // TODO(tim): consider non-null ratio in histograms
  // stream << "  non-null " << non_null_value_ratio() << std::endl;
  stream << "  bins        " << bin_count() << std::endl;

  stream << "  edges / counts " << std::endl;
  for (BinID bin = 0u; bin < bin_count(); bin++) {
    stream << "              [" << _bin_minimum(bin) << ", " << _bin_maximum(bin) << "]: ";
    stream << _bin_height(bin) << std::endl;
  }

  return stream.str();
}

template <typename T>
std::vector<std::pair<T, HistogramCountType>> AbstractHistogram<T>::_gather_value_distribution(
    const std::shared_ptr<const BaseSegment>& segment) {
  std::map<T, HistogramCountType> value_counts;

  resolve_segment_type<T>(*segment, [&](auto& typed_segment) {
    auto iterable = create_iterable_from_segment<T>(typed_segment);
    iterable.for_each([&](const auto& value) {
      if (!value.is_null()) {
        value_counts[value.value()]++;
      }
    });
  });

  std::vector<std::pair<T, HistogramCountType>> result(value_counts.cbegin(), value_counts.cend());
  return result;
}

template <typename T>
T AbstractHistogram<T>::minimum() const {
  return _bin_minimum(0u);
}

template <typename T>
T AbstractHistogram<T>::maximum() const {
  return _bin_maximum(bin_count() - 1u);
}

template <>
uint64_t AbstractHistogram<std::string>::_convert_string_to_number_representation(const std::string& value) const {
  return convert_string_to_number_representation(value, _supported_characters, _string_prefix_length);
}

template <>
std::string AbstractHistogram<std::string>::_convert_number_representation_to_string(const uint64_t value) const {
  return convert_number_representation_to_string(value, _supported_characters, _string_prefix_length);
}

template <typename T>
typename AbstractHistogram<T>::HistogramWidthType AbstractHistogram<T>::_bin_width(const BinID index) const {
  DebugAssert(index < bin_count(), "Index is not a valid bin.");
  return _get_next_value(_bin_maximum(index) - _bin_minimum(index));
}

template <>
AbstractHistogram<std::string>::HistogramWidthType AbstractHistogram<std::string>::_bin_width(const BinID index) const {
  DebugAssert(index < bin_count(), "Index is not a valid bin.");

  const auto repr_min = _convert_string_to_number_representation(_bin_minimum(index));
  const auto repr_max = _convert_string_to_number_representation(_bin_maximum(index));
  return repr_max - repr_min + 1u;
}

template <typename T>
T AbstractHistogram<T>::_get_next_value(const T value) const {
  if constexpr (std::is_same_v<T, std::string>) {
    return next_value(value, _supported_characters);
  } else {
    return next_value(value);
  }
}

template <typename T>
float AbstractHistogram<T>::_share_of_bin_less_than_value(const BinID bin_id, const T value) const {
  /**
   * Returns the share of values smaller than `value` in the given bin.
   *
   * We need to convert strings to their numerical representation to calculate a share.
   * This conversion is done based on prefixes because strings of arbitrary length cannot be converted to a numerical
   * representation that satisfies the following requirements:
   *  1. For two strings s1 and s2: s1 < s2 -> repr(s1) < repr(s2)
   *  2. For two strings s1 and s2: dist(s1, s2) == repr(s2) - repr(s1)
   *  repr(s) is the numerical representation for a string s, and dist(s1, s2) returns the number of strings between
   *  s1 and s2 in the domain of strings with at most length `string_prefix_length`
   *  and the set of supported characters `supported_characters`.
   *
   * Thus, we calculate the range based only on a domain of strings with a maximum length of `string_prefix_length`
   * characters.
   * However, we make use of a trick: if the bin edges share a common prefix, we strip that common prefix and
   * take the substring starting after that prefix.
   *
   * Example:
   *  - bin: ["intelligence", "intellij"]
   *  - supported_characters: [a-z]
   *  - string_prefix_length: 4
   *  - value: intelligent
   *
   *  Traditionally, if we did not strip the common prefix, we would calculate the range based on the
   *  substring of length `string_prefix_length`, which is "inte" for both lower and upper edge of the bin.
   *  We could not make a reasonable assumption how large the share is.
   *  Instead, we strip the common prefix ("intelli") and calculate the share based on the numerical representation
   *  of the substring after the common prefix.
   *  That is, what is the share of values smaller than "gent" in the range ["gence", "j"]?
   */
  if constexpr (!std::is_same_v<T, std::string>) {
    return static_cast<float>(value - _bin_minimum(bin_id)) / _bin_width(bin_id);
  } else {
    const auto bin_min = _bin_minimum(bin_id);
    const auto bin_max = _bin_maximum(bin_id);
    const auto common_prefix_len = common_prefix_length(bin_min, bin_max);

    DebugAssert(value.substr(0, common_prefix_len) == bin_min.substr(0, common_prefix_len),
                "Value does not belong to bin");

    const auto value_repr = _convert_string_to_number_representation(value.substr(common_prefix_len));
    const auto min_repr = _convert_string_to_number_representation(bin_min.substr(common_prefix_len));
    const auto max_repr = _convert_string_to_number_representation(bin_max.substr(common_prefix_len));
    return static_cast<float>(value_repr - min_repr) / (max_repr - min_repr + 1);
  }
}

template <typename T>
bool AbstractHistogram<T>::_does_not_contain(const PredicateCondition predicate_type,
                                             const AllTypeVariant& variant_value,
                                             const std::optional<AllTypeVariant>& variant_value2) const {
  const auto value = type_cast<T>(variant_value);

  switch (predicate_type) {
    case PredicateCondition::Equals: {
      const auto bin_id = _bin_for_value(value);
      // It is possible for EqualWidthHistograms to have empty bins.
      return bin_id == INVALID_BIN_ID || _bin_height(bin_id) == 0ul;
    }
    case PredicateCondition::NotEquals:
      return minimum() == value && maximum() == value;
    case PredicateCondition::LessThan:
      return value <= minimum();
    case PredicateCondition::LessThanEquals:
      return value < minimum();
    case PredicateCondition::GreaterThanEquals:
      return value > maximum();
    case PredicateCondition::GreaterThan:
      return value >= maximum();
    case PredicateCondition::Between: {
      Assert(static_cast<bool>(variant_value2), "Between operator needs two values.");

      if (does_not_contain(PredicateCondition::GreaterThanEquals, value)) {
        return true;
      }

      const auto value2 = type_cast<T>(*variant_value2);
      if (does_not_contain(PredicateCondition::LessThanEquals, value2) || value2 < value) {
        return true;
      }

      const auto value_bin = _bin_for_value(value);
      const auto value2_bin = _bin_for_value(value2);

      // In an EqualDistinctCountHistogram, if both values fall into the same gap, we can prune the predicate.
      // We need to have at least two bins to rule out pruning if value < min and value2 > max.
      if (value_bin == INVALID_BIN_ID && value2_bin == INVALID_BIN_ID && bin_count() > 1ul &&
          _next_bin_for_value(value) == _next_bin_for_value(value2)) {
        return true;
      }

      // In an EqualWidthHistogram, if both values fall into a bin that has no elements,
      // and there are either no bins in between or none of them have any elements, we can also prune the predicate.
      if (value_bin != INVALID_BIN_ID && value2_bin != INVALID_BIN_ID && _bin_height(value_bin) == 0 &&
          _bin_height(value2_bin) == 0) {
        for (auto current_bin = value_bin + 1; current_bin < value2_bin; current_bin++) {
          if (_bin_height(current_bin) > 0ul) {
            return false;
          }
        }
        return true;
      }

      return false;
    }
    case PredicateCondition::Like:
    case PredicateCondition::NotLike:
      Fail("Predicate (NOT) LIKE is not supported for non-string columns.");
    default:
      // Do not prune predicates we cannot (yet) handle.
      return false;
  }
}

template <typename T>
bool AbstractHistogram<T>::does_not_contain(const PredicateCondition predicate_type,
                                            const AllTypeVariant& variant_value,
                                            const std::optional<AllTypeVariant>& variant_value2) const {
  return _does_not_contain(predicate_type, variant_value, variant_value2);
}

template <>
bool AbstractHistogram<std::string>::does_not_contain(const PredicateCondition predicate_type,
                                                      const AllTypeVariant& variant_value,
                                                      const std::optional<AllTypeVariant>& variant_value2) const {
  const auto value = type_cast<std::string>(variant_value);

  // Only allow supported characters in search value.
  // If predicate is (NOT) LIKE additionally allow wildcards.
  const auto allowed_characters =
      _supported_characters +
      (predicate_type == PredicateCondition::Like || predicate_type == PredicateCondition::NotLike ? "_%" : "");
  Assert(value.find_first_not_of(allowed_characters) == std::string::npos, "Unsupported characters.");

  switch (predicate_type) {
    case PredicateCondition::Like: {
      if (!LikeMatcher::contains_wildcard(value)) {
        return does_not_contain(PredicateCondition::Equals, value);
      }

      // If the pattern starts with a MatchAll, we can not prune it.
      if (value.front() == '%') {
        return false;
      }

      /**
       * We can prune prefix searches iff the domain of values captured by a prefix pattern is prunable.
       *
       * Example:
       * bins: [a, b], [d, e]
       * predicate: col LIKE 'c%'
       *
       * With the same argument we can also prune predicates in the form of 'c%foo',
       * where foo can be any pattern itself.
       * We only have to consider the pattern up to the first AnyChars wildcard.
       */
      const auto match_all_index = value.find('%');
      if (match_all_index != std::string::npos) {
        const auto search_prefix = value.substr(0, match_all_index);
        if (does_not_contain(PredicateCondition::GreaterThanEquals, search_prefix)) {
          return true;
        }

        const auto search_prefix_next_value = next_value(search_prefix, _supported_characters, search_prefix.length());

        // If the next value is the same as the prefix, it means that there is no larger value in the domain
        // of substrings. In that case we cannot prune, because otherwise we previous check would already return true.
        if (search_prefix == search_prefix_next_value) {
          return false;
        }

        if (does_not_contain(PredicateCondition::LessThan, search_prefix_next_value)) {
          return true;
        }

        const auto search_prefix_bin = _bin_for_value(search_prefix);
        const auto search_prefix_next_value_bin = _bin_for_value(search_prefix_next_value);

        if (search_prefix_bin == INVALID_BIN_ID) {
          const auto search_prefix_next_bin = _next_bin_for_value(search_prefix);

          // In an EqualDistinctCountHistogram, if both values fall into the same gap, we can prune the predicate.
          // We need to have at least two bins to rule out pruning if search_prefix < min
          // and search_prefix_next_value > max.
          if (search_prefix_next_value_bin == INVALID_BIN_ID && bin_count() > 1ul &&
              search_prefix_next_bin == _next_bin_for_value(search_prefix_next_value)) {
            return true;
          }

          // In an EqualDistinctCountHistogram, if the search_prefix_next_value is exactly the lower bin edge of
          // the upper bound of search_prefix, we can also prune.
          // That's because search_prefix_next_value does not belong to the range covered by the pattern,
          // but is the next value after it.
          if (search_prefix_next_value_bin != INVALID_BIN_ID &&
              search_prefix_next_bin == search_prefix_next_value_bin &&
              _bin_minimum(search_prefix_next_value_bin) == search_prefix_next_value) {
            return true;
          }
        }

        // In an EqualWidthHistogram, if both values fall into a bin that has no elements,
        // and there are either no bins in between or none of them have any elements, we can also prune the predicate.
        // If the count of search_prefix_next_value_bin is not 0 but search_prefix_next_value is the lower bin edge,
        // we can still prune, because search_prefix_next_value is not part of the range (same as above).
        if (search_prefix_bin != INVALID_BIN_ID && search_prefix_next_value_bin != INVALID_BIN_ID &&
            _bin_height(search_prefix_bin) == 0u &&
            (_bin_height(search_prefix_next_value_bin) == 0u ||
             _bin_minimum(search_prefix_next_value_bin) == search_prefix_next_value)) {
          for (auto current_bin = search_prefix_bin + 1; current_bin < search_prefix_next_value_bin; current_bin++) {
            if (_bin_height(current_bin) > 0u) {
              return false;
            }
          }
          return true;
        }

        return false;
      }

      return false;
    }
    case PredicateCondition::NotLike: {
      if (!LikeMatcher::contains_wildcard(value)) {
        return does_not_contain(PredicateCondition::NotEquals, variant_value);
      }

      // If the pattern starts with a MatchAll, we can only prune it if it matches all values.
      if (value.front() == '%') {
        return value == "%";
      }

      /**
       * We can also prune prefix searches iff the domain of values captured by the histogram is less than or equal to
       * the domain of strings captured by a prefix pattern.
       *
       * Example:
       * min: car
       * max: crime
       * predicate: col NOT LIKE 'c%'
       *
       * With the same argument we can also prune predicates in the form of 'c%foo',
       * where foo can be any pattern itself.
       * We only have to consider the pattern up to the first MatchAll character.
       */
      const auto match_all_index = value.find('%');
      if (match_all_index != std::string::npos) {
        const auto search_prefix = value.substr(0, match_all_index);
        if (search_prefix == minimum().substr(0, search_prefix.length()) &&
            search_prefix == maximum().substr(0, search_prefix.length())) {
          return true;
        }
      }

      return false;
    }
    default:
      return _does_not_contain(predicate_type, variant_value, variant_value2);
  }
}

template <typename T>
std::pair<float, bool> AbstractHistogram<T>::_estimate_cardinality(
    const PredicateCondition predicate_type, const AllTypeVariant& variant_value,
    const std::optional<AllTypeVariant>& variant_value2) const {
  if (does_not_contain(predicate_type, variant_value, variant_value2)) {
    return {0.f, true};
  }

  const auto value = type_cast<T>(variant_value);

  switch (predicate_type) {
    case PredicateCondition::Equals: {
      const auto index = _bin_for_value(value);
      const auto bin_count_distinct = _bin_distinct_count(index);

      // This should never be false because does_not_contain should have been true further up if this was the case.
      DebugAssert(bin_count_distinct > 0u, "0 distinct values in bin.");

      return {static_cast<float>(_bin_height(index)) / static_cast<float>(bin_count_distinct),
              bin_count_distinct == 1u ? true : false};
    }
    case PredicateCondition::NotEquals: {
      const auto estimate_pair = _estimate_cardinality(PredicateCondition::Equals, variant_value);
      return {total_count() - estimate_pair.first, estimate_pair.second};
    }
    case PredicateCondition::LessThan: {
      if (value > maximum()) {
        return {total_count(), true};
      }

      // This should never be false because does_not_contain should have been true further up if this was the case.
      DebugAssert(value >= minimum(), "Value smaller than min of histogram.");

      auto index = _bin_for_value(value);
      auto cardinality = 0.f;

      auto estimate_is_certain = false;

      if (index == INVALID_BIN_ID) {
        // The value is within the range of the histogram, but does not belong to a bin.
        // Therefore, we need to sum up the counts of all bins with a max < value.
        index = _next_bin_for_value(value);
        estimate_is_certain = true;
      } else {
        cardinality += _share_of_bin_less_than_value(index, value) * _bin_height(index);
      }

      // Sum up all bins before the bin (or gap) containing the value.
      for (BinID bin = 0u; bin < index; bin++) {
        cardinality += _bin_height(bin);
      }

      /**
       * The cardinality is capped at total_count().
       * It is possible for a value that is smaller than or equal to the max of the EqualHeightHistogram
       * to yield a calculated cardinality higher than total_count.
       * This is due to the way EqualHeightHistograms store the count for a bin,
       * which is in a single value (count_per_bin) for all bins rather than a vector (one value for each bin).
       * Consequently, this value is the desired count for all bins.
       * In practice, _bin_count(n) >= _count_per_bin for n < bin_count() - 1,
       * because bins are filled up until the count is at least _count_per_bin.
       * The last bin typically has a count lower than _count_per_bin.
       * Therefore, if we calculate the share of the last bin based on _count_per_bin
       * we might end up with an estimate higher than total_count(), which is then capped.
       */
      return {std::min(cardinality, static_cast<float>(total_count())), estimate_is_certain};
    }
    case PredicateCondition::LessThanEquals:
      return estimate_cardinality(PredicateCondition::LessThan, _get_next_value(value));
    case PredicateCondition::GreaterThanEquals: {
      const auto estimate_pair = estimate_cardinality(PredicateCondition::LessThan, variant_value);
      return {total_count() - estimate_pair.first, estimate_pair.second};
    }
    case PredicateCondition::GreaterThan: {
      const auto estimate_pair = estimate_cardinality(PredicateCondition::LessThanEquals, variant_value);
      return {total_count() - estimate_pair.first, estimate_pair.second};
    }
    case PredicateCondition::Between: {
      Assert(static_cast<bool>(variant_value2), "Between operator needs two values.");
      const auto value2 = type_cast<T>(*variant_value2);

      if (value2 < value) {
        return {0.f, true};
      }

      const auto estimate_pair_lte_value2 = estimate_cardinality(PredicateCondition::LessThanEquals, *variant_value2);
      const auto estimate_pair_lt_value = estimate_cardinality(PredicateCondition::LessThan, variant_value);
      return {estimate_pair_lte_value2.first - estimate_pair_lt_value.first,
              estimate_pair_lte_value2.second && estimate_pair_lt_value.second};
    }
    case PredicateCondition::Like:
    case PredicateCondition::NotLike:
      Fail("Predicate NOT LIKE is not supported for non-string columns.");
    default:
      // TODO(anyone): implement more meaningful things here
      return {total_count(), false};
  }
}

// Specialization for numbers.
template <typename T>
std::pair<float, bool> AbstractHistogram<T>::estimate_cardinality(
    const PredicateCondition predicate_type, const AllTypeVariant& variant_value,
    const std::optional<AllTypeVariant>& variant_value2) const {
  return _estimate_cardinality(predicate_type, variant_value, variant_value2);
}

// Specialization for strings.
template <>
std::pair<float, bool> AbstractHistogram<std::string>::estimate_cardinality(
    const PredicateCondition predicate_type, const AllTypeVariant& variant_value,
    const std::optional<AllTypeVariant>& variant_value2) const {
  const auto value = type_cast<std::string>(variant_value);

  // Only allow supported characters in search value.
  // If predicate is (NOT) LIKE additionally allow wildcards.
  const auto allowed_characters =
      _supported_characters +
      (predicate_type == PredicateCondition::Like || predicate_type == PredicateCondition::NotLike ? "_%" : "");
  Assert(value.find_first_not_of(allowed_characters) == std::string::npos, "Unsupported characters.");

  if (does_not_contain(predicate_type, variant_value, variant_value2)) {
    return {0.f, true};
  }

  switch (predicate_type) {
    case PredicateCondition::Like: {
      if (!LikeMatcher::contains_wildcard(value)) {
        return estimate_cardinality(PredicateCondition::Equals, variant_value);
      }

      // We don't deal with this for now because it is not worth the effort.
      // TODO(anyone): think about good way to handle SingleChar wildcard in patterns.
      const auto single_char_count = std::count(value.cbegin(), value.cend(), '_');
      if (single_char_count > 0u) {
        return {total_count(), false};
      }

      const auto any_chars_count = std::count(value.cbegin(), value.cend(), '%');
      DebugAssert(any_chars_count > 0u,
                  "contains_wildcard() should not return true if there is neither a '%' nor a '_' in the string.");

      // Match everything.
      if (value == "%") {
        return {total_count(), true};
      }

      if (value.front() != '%') {
        /**
         * We know now we have some sort of prefix search, because there is at least one AnyChars wildcard,
         * and it is not at the start of the pattern.
         *
         * We differentiate two cases:
         *  1. Simple prefix searches, e.g., 'foo%', where there is exactly one AnyChars wildcard in the pattern,
         *  and it is at the end of the pattern.
         *  2. All others, e.g., 'foo%bar' or 'foo%bar%'.
         *
         *  The way we handle these cases is we only estimate simple prefix patterns and assume uniform distribution
         *  for additional fixed characters for the second case.
         *  Note: this is obviously far from great because not only do characters not appear with equal probability,
         *  they also appear with different probability depending on characters around them.
         *  The combination 'ing' in English is far more likely than 'qzy'.
         *  One improvement would be to have a frequency table for characters and take the probability from there,
         *  but it only gets you so far. It does not help with the second property.
         *  Nevertheless, it could be helpful especially if the number of actually occurring characters in a column are
         *  small compared to the supported characters and the frequency table would be not static but built during
         *  histogram generation.
         *  TODO(anyone): look into that in more detail.
         *
         *  That is, to estimate the first case ('foo%'), we calculate
         *  estimate_cardinality(LessThan, fop) - estimate_cardinaliy(LessThan, foo).
         *  That covers all strings starting with foo.
         *
         *  In the second case we assume that all characters in _supported_characters are equally likely to appear in
         *  a string, and therefore divide the above cardinality by the number of supported characters for each
         *  additional character that is fixed in the string after the prefix.
         *
         *  Example for 'foo%bar%baz', if we only supported the 26 lowercase latin characters:
         *  (estimate_cardinality(LessThan, fop) - estimate_cardinality(LessThan, foo)) / 26^6
         *  There are six additional fixed characters in the string ('b', 'a', 'r', 'b', 'a', and 'z').
         */
        const auto search_prefix = value.substr(0, value.find('%'));
        auto additional_characters = value.length() - search_prefix.length() - any_chars_count;

        // If there are too many fixed characters for the power to be calculated without overflow, cap the exponent.
        const auto maximum_exponent =
            std::log(std::numeric_limits<uint64_t>::max()) / std::log(_supported_characters.length());
        if (additional_characters > maximum_exponent) {
          additional_characters = static_cast<uint64_t>(maximum_exponent);
        }

        const auto search_prefix_next_value = next_value(search_prefix, _supported_characters, search_prefix.length());

        // If the next value is the same as the prefix, it means that there is no larger value in the domain
        // of substrings. In that case all values (total_count()) are smaller than search_prefix_next_value.
        const auto count_smaller_next_value =
            search_prefix == search_prefix_next_value
                ? total_count()
                : estimate_cardinality(PredicateCondition::LessThan, search_prefix_next_value).first;

        return {(count_smaller_next_value - estimate_cardinality(PredicateCondition::LessThan, search_prefix).first) /
                    ipow(_supported_characters.length(), additional_characters),
                false};
      }

      /**
       * If we do not have a prefix search, but a suffix or contains search, the prefix histograms do not help us.
       * We simply assume uniform distribution for all supported characters and divide the total number of rows
       * by the number of supported characters for each additional character that is fixed (see comment above).
       *
       * Example for '%foo%b%a%', if we only supported the 26 lowercase latin characters:
       * total_count() / 26^5
       * There are five fixed characters in the string ('f', 'o', 'o', 'b', and 'a').
       */
      const auto fixed_characters = value.length() - any_chars_count;
      return {static_cast<float>(total_count()) / ipow(_supported_characters.length(), fixed_characters), false};
    }
    case PredicateCondition::NotLike: {
      if (!LikeMatcher::contains_wildcard(value)) {
        return estimate_cardinality(PredicateCondition::NotEquals, variant_value);
      }

      // We don't deal with this for now because it is not worth the effort.
      // TODO(anyone): think about good way to handle SingleChar wildcard in patterns.
      const auto single_char_count = std::count(value.cbegin(), value.cend(), '_');
      if (single_char_count > 0u) {
        return {total_count(), false};
      }

      const auto estimate_pair = estimate_cardinality(PredicateCondition::Like, variant_value);
      return {total_count() - estimate_pair.first, estimate_pair.second};
    }
    default:
      return _estimate_cardinality(predicate_type, variant_value, variant_value2);
  }
}

template <typename T>
std::pair<float, bool> AbstractHistogram<T>::estimate_selectivity(
    const PredicateCondition predicate_type, const AllTypeVariant& variant_value,
    const std::optional<AllTypeVariant>& variant_value2) const {
  const auto estimate_pair = estimate_cardinality(predicate_type, variant_value, variant_value2);
  return {estimate_pair.first / total_count(), estimate_pair.second};
}

template <typename T>
std::shared_ptr<AbstractStatisticsObject> AbstractHistogram<T>::slice_with_predicate(
    const PredicateCondition predicate_type, const AllTypeVariant& variant_value,
    const std::optional<AllTypeVariant>& variant_value2) const {
  if (does_not_contain(predicate_type, variant_value, variant_value2)) {
    Fail("TODO");
  }

  const auto value = type_cast<T>(variant_value);

  std::vector<T> bin_minima;
  std::vector<T> bin_maxima;
  std::vector<HistogramCountType> bin_heights;
  std::vector<HistogramCountType> bin_distinct_counts;

  switch (predicate_type) {
    case PredicateCondition::Equals: {
      bin_minima.emplace_back(value);
      bin_maxima.emplace_back(value);
      bin_heights.emplace_back(static_cast<HistogramCountType>(
          std::ceil(estimate_cardinality(PredicateCondition::Equals, variant_value).first)));
      bin_distinct_counts.emplace_back(1);
    } break;

    case PredicateCondition::NotEquals: {
      const auto value_bin_id = _bin_for_value(value);

      // Do not create empty bin.
      const auto new_bin_count = _bin_distinct_count(value_bin_id) == 1u ? bin_count() - 1 : bin_count();

      bin_minima.resize(new_bin_count);
      bin_maxima.resize(new_bin_count);
      bin_heights.resize(new_bin_count);
      bin_distinct_counts.resize(new_bin_count);

      for (auto bin_id = BinID{0}; bin_id < new_bin_count; ++bin_id) {
        // TODO(anyone) we currently do not manipulate the bin bounds if `variant_value` equals such a bound. We would
        //              expect the accuracy improvement to be minimal, if we did. Also, this would be hard to do for
        //              strings.
        bin_minima[bin_id] = _bin_minimum(bin_id);
        bin_maxima[bin_id] = _bin_maximum(bin_id);

        if (bin_id == value_bin_id) {
          const auto distinct_count = _bin_distinct_count(bin_id);

          // Do not create empty bin.
          if (distinct_count == 1) {
            continue;
          }

          const auto value_count = static_cast<HistogramCountType>(
              std::ceil(estimate_cardinality(PredicateCondition::Equals, variant_value, variant_value2).first));

          bin_heights[bin_id] = _bin_height(bin_id) - value_count;
          bin_distinct_counts[bin_id] = distinct_count - 1;
        } else {
          bin_heights[bin_id] = _bin_height(bin_id);
          bin_distinct_counts[bin_id] = _bin_distinct_count(bin_id);
        }
      }
    } break;

    case PredicateCondition::LessThan:
    case PredicateCondition::LessThanEquals: {
      const auto bin_for_value = _bin_for_value(value);

      BinID sliced_bin_count;
      if (bin_for_value == INVALID_BIN_ID) {
        // If the value does not belong to a bin, we need to differentiate between values greater than the maximum
        // of the histogram and all other values. If the value is greater than the maximum, return a copy of itself.
        // Otherwise, we include all bins before to the bin of that value.
        const auto next_bin_for_value = _next_bin_for_value(value);

        if (next_bin_for_value == INVALID_BIN_ID) {
          return clone();
        } else {
          sliced_bin_count = next_bin_for_value;
        }
      } else if (predicate_type == PredicateCondition::LessThan && value == _bin_minimum(bin_for_value)) {
        // If the predicate is LessThan and the value is the lower edge of a bin, we do not need to include that bin.
        sliced_bin_count = bin_for_value;
      } else {
        sliced_bin_count = bin_for_value + 1;
      }

      DebugAssert(sliced_bin_count > 0, "This should have been caught by does_not_contain().");

      bin_minima.resize(sliced_bin_count);
      bin_maxima.resize(sliced_bin_count);
      bin_heights.resize(sliced_bin_count);
      bin_distinct_counts.resize(sliced_bin_count);

      // If value is not in a gap, calculate the share of the bin to slice. Otherwise take the whole bin.
      BinID last_sliced_bin_id = sliced_bin_count - 1;
      if (value < _bin_maximum(last_sliced_bin_id)) {
        bin_minima.back() = _bin_minimum(last_sliced_bin_id);
        // TODO(anyone): this could be previous_value(value) for LessThan, but this is not available for strings
        // and we do not expect it to make a big difference.
        bin_maxima.back() = value;

        const auto less_than_bound = predicate_type == PredicateCondition::LessThan ? value : _get_next_value(value);
        const auto sliced_bin_share = _share_of_bin_less_than_value(last_sliced_bin_id, less_than_bound);
        bin_heights.back() =
            static_cast<HistogramCountType>(std::ceil(_bin_height(last_sliced_bin_id) * sliced_bin_share));
        bin_distinct_counts.back() =
            static_cast<HistogramCountType>(std::ceil(_bin_distinct_count(last_sliced_bin_id) * sliced_bin_share));
      } else {
        last_sliced_bin_id = sliced_bin_count;
      }

      for (auto bin_id = BinID{0}; bin_id < last_sliced_bin_id; ++bin_id) {
        bin_minima[bin_id] = _bin_minimum(bin_id);
        bin_maxima[bin_id] = _bin_maximum(bin_id);
        bin_heights[bin_id] = _bin_height(bin_id);
        bin_distinct_counts[bin_id] = _bin_distinct_count(bin_id);
      }
    } break;

    case PredicateCondition::GreaterThan:
    case PredicateCondition::GreaterThanEquals: {
      const auto bin_for_value = _bin_for_value(value);

      BinID sliced_bin_count;
      if (bin_for_value == INVALID_BIN_ID) {
        // If the value does not belong to a bin, we need to differentiate between values greater than the maximum
        // of the histogram and all other values. If the value is greater than the maximum, we have no matches.
        // Otherwise, we include all bins before the bin of that value.
        const auto next_bin_for_value = _next_bin_for_value(value);

        if (next_bin_for_value == INVALID_BIN_ID) {
          sliced_bin_count = 0;
        } else if (next_bin_for_value == 0) {
          return clone();
        } else {
          sliced_bin_count = bin_count() - next_bin_for_value;
        }
      } else if (predicate_type == PredicateCondition::GreaterThan && value == _bin_maximum(bin_for_value)) {
        // If the predicate is GreaterThan and the value is the upper edge of a bin, we do not need to include that bin.
        sliced_bin_count = bin_count() - bin_for_value - 1;
      } else {
        sliced_bin_count = bin_count() - bin_for_value;
      }

      DebugAssert(sliced_bin_count > 0, "This should have been caught by does_not_contain().");

      const auto first_sliced_bin_id = BinID{bin_count() - sliced_bin_count};

      bin_minima.resize(sliced_bin_count);
      bin_maxima.resize(sliced_bin_count);
      bin_heights.resize(sliced_bin_count);
      bin_distinct_counts.resize(sliced_bin_count);

      bin_maxima.front() = _bin_maximum(first_sliced_bin_id);

      // If value is not in a gap, calculate the share of the bin to slice. Otherwise take the whole bin.
      if (value > _bin_minimum(first_sliced_bin_id)) {
        bin_minima.front() = predicate_type == PredicateCondition::GreaterThan ? _get_next_value(value) : value;

        // For GreaterThan, `_get_previous_value(value)` would be more correct, but we don't have that for strings
        const auto sliced_bin_share = 1.0f - _share_of_bin_less_than_value(first_sliced_bin_id, value);

        bin_heights.front() =
            static_cast<HistogramCountType>(std::ceil(_bin_height(first_sliced_bin_id) * sliced_bin_share));
        bin_distinct_counts.front() =
            static_cast<HistogramCountType>(std::ceil(_bin_distinct_count(first_sliced_bin_id) * sliced_bin_share));
      } else {
        bin_minima.front() = _bin_minimum(first_sliced_bin_id);
        bin_heights.front() = _bin_height(first_sliced_bin_id);
        bin_distinct_counts.front() = _bin_distinct_count(first_sliced_bin_id);
      }

      const auto first_complete_bin_id = BinID{bin_count() - sliced_bin_count + 1};
      for (auto bin_id = first_complete_bin_id; bin_id < bin_count(); ++bin_id) {
        const auto sliced_bin_id = bin_id - first_complete_bin_id + 1;
        bin_minima[sliced_bin_id] = _bin_minimum(bin_id);
        bin_maxima[sliced_bin_id] = _bin_maximum(bin_id);
        bin_heights[sliced_bin_id] = _bin_height(bin_id);
        bin_distinct_counts[sliced_bin_id] = _bin_distinct_count(bin_id);
      }
    } break;

    case PredicateCondition::Between:
      Assert(variant_value2, "BETWEEN needs a second value.");
      return slice_with_predicate(PredicateCondition::GreaterThanEquals, variant_value)
          ->slice_with_predicate(PredicateCondition::LessThanEquals, *variant_value2);

    case PredicateCondition::Like:
    case PredicateCondition::NotLike:
      Fail("PredicateCondition not yet supported by Histograms");

    case PredicateCondition::In:
    case PredicateCondition::IsNull:
    case PredicateCondition::IsNotNull:
      Fail("PredicateCondition not supported by Histograms");
  }

  return std::make_shared<GenericHistogram<T>>(std::move(bin_minima), std::move(bin_maxima), std::move(bin_heights),
                                               std::move(bin_distinct_counts));
}

EXPLICITLY_INSTANTIATE_DATA_TYPES(AbstractHistogram);

}  // namespace opossum
