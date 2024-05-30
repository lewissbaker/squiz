///////////////////////////////////////////////////////////////////////////////
// Squiz
// Copyright 2024, Toyota Motor Corporation
// Licensed under Apache License 2.0 with LLVM Exceptions.
///////////////////////////////////////////////////////////////////////////////
#pragma once

#include <algorithm>
#include <bit>
#include <cassert>
#include <concepts>
#include <cstdint>
#include <type_traits>
#include <utility>

#include <squiz/detail/bitset.hpp>
#include <squiz/detail/intrusive_list.hpp>
#include <squiz/detail/smallest_unsigned_integer.hpp>

namespace squiz::detail {

/// A data-structure for efficiently maintaining a large number of
/// intrusively-linked items, each having a 'due_time', allowing adding new
/// items, removing items and dequeuing the earliest due item in amortised
/// constant time.
///
/// Uses a hierarchical
///
// clang-format off
template <
    typename Item,
    Item* Item::* Next,
    Item* Item::* Prev,
    std::unsigned_integral Timestamp,
    std::move_constructible GetTimestampFunc,
  Timestamp MaxTimestamp = std::numeric_limits<Timestamp>::max(),
  std::uint32_t MinorBinsPerMajorBin = 256>
  requires std::is_nothrow_invocable_r_v<Timestamp, const GetTimestampFunc&, const Item*>
class binned_time_priority_queue {
  // clang-format on
  static_assert(MaxTimestamp > 0);
  static_assert(std::has_single_bit(MinorBinsPerMajorBin));

public:
  using item_list = intrusive_list<Item, Next, Prev>;

  binned_time_priority_queue() noexcept(
      std::is_nothrow_default_constructible_v<GetTimestampFunc> &&
      std::is_nothrow_move_constructible_v<GetTimestampFunc>)
    requires std::default_initializable<GetTimestampFunc>
    : binned_time_priority_queue(GetTimestampFunc()) {}

  explicit binned_time_priority_queue(GetTimestampFunc get_timestamp) noexcept(
      std::is_nothrow_move_constructible_v<GetTimestampFunc>);

  binned_time_priority_queue(binned_time_priority_queue&& other) noexcept(
      std::is_nothrow_move_constructible_v<GetTimestampFunc>) = default;

  static constexpr Timestamp max_timestamp() noexcept { return MaxTimestamp; }

  /// Compute a due-time such that no items in the queue have a due-time before
  /// this time.
  ///
  /// Note that the result may be approximate in that for due-times further into
  /// the future (i.e. not represented by bins in the first major bin) the
  /// returned due time will potentially be earlier than the actual due-time of
  /// an item by an amount equal to the granularity of the minor min size in
  /// which the next item lives. As the current time advances and due-times get
  /// closer, the
  ///
  /// If there are no items in the queue then returns MaxTimestamp.
  ///
  /// The try_dequeue_next_due() function will not return any items until the
  /// current time has at least advanced beyond the returned time.
  Timestamp earliest_due_time_lower_bound() const noexcept;

  /// Add an item to the queue.
  void add(Item* x) noexcept;

  /// Remove an item from the queue.
  void remove(Item* x) noexcept;

  /// Try to dequeue the earliest due item from the queue.
  ///
  /// If there are no items in the queue for which their due timestamp is less
  /// than or equal to \c current_time then returns \c nullptr.
  Item* try_dequeue_next_due_by(Timestamp current_time) noexcept;

private:
  static constexpr std::uint32_t minor_bins_per_major_bin =
      MinorBinsPerMajorBin;

  // Note: Passing 'count' here instead of 'count-1' since we want to be able to
  // use 'count' to represent 'no active bin'.
  using minor_bin_index_t =
      smallest_unsigned_integer_for<minor_bins_per_major_bin>;
  static constexpr minor_bin_index_t no_active_bin = minor_bins_per_major_bin;
  using major_bin_index_t = std::uint32_t;

  static constexpr minor_bin_index_t minor_index_mask =
      (minor_bins_per_major_bin - 1);

  static constexpr Timestamp active_time_mask =
      ~static_cast<Timestamp>(minor_index_mask);

  static constexpr std::uint32_t bits_per_major_bin =
      std::bit_width(minor_bins_per_major_bin - 1);
  static constexpr std::uint32_t major_bin_count =
      (std::bit_width(MaxTimestamp) + (bits_per_major_bin - 1)) /
      bits_per_major_bin;
  static constexpr std::uint32_t total_bin_count =
      major_bin_count * minor_bins_per_major_bin;

  struct bin_index {
    major_bin_index_t major;
    minor_bin_index_t minor;

    static bin_index from_linear_index(std::size_t linear_index) noexcept {
      assert(linear_index < total_bin_count);
      return bin_index{
          static_cast<major_bin_index_t>(
              linear_index / minor_bins_per_major_bin),
          static_cast<minor_bin_index_t>(
              linear_index % minor_bins_per_major_bin)};
    }

    std::size_t linear_index() const noexcept {
      return static_cast<std::size_t>(major) * minor_bins_per_major_bin + minor;
    }
  };

  Timestamp get_item_due_time(const Item* item) const noexcept {
    return std::invoke(get_timestamp_, item);
  }

  bin_index get_bin_index(Timestamp time) const noexcept {
    assert(time >= active_time_);
    assert(time <= MaxTimestamp);

    const Timestamp diff = time ^ active_time_;
    const std::uint32_t diff_bits = std::max(std::bit_width(diff), 1) - 1;
    const std::uint32_t major_bin_index = diff_bits / bits_per_major_bin;
    assert(major_bin_index < major_bin_count);

    const std::uint32_t bin_shift = major_bin_index * bits_per_major_bin;
    const std::uint32_t minor_bin_index =
        (time >> bin_shift) & minor_index_mask;
    assert(minor_bin_index < minor_bins_per_major_bin);

    return bin_index{
        static_cast<major_bin_index_t>(major_bin_index),
        static_cast<minor_bin_index_t>(minor_bin_index)};
  }

  bin_index get_bin_index(const Item* item) const noexcept {
    return get_bin_index(get_item_due_time(item));
  }

  Timestamp get_bin_time(bin_index index) const noexcept {
    const Timestamp bin_duration = Timestamp(1)
        << (index.major * bits_per_major_bin);
    const Timestamp time_mask_low_bits =
        (bin_duration << bits_per_major_bin) - 1;
    const Timestamp time_mask_high_bits = ~time_mask_low_bits;

    return (active_time_ & time_mask_high_bits) + bin_duration * index.minor;
  }

  item_list& get_bin(bin_index index) noexcept {
    return bins_[index.linear_index()];
  }

  const item_list& get_bin(bin_index index) const noexcept {
    return bins_[index.linear_index()];
  }

  __attribute__((noinline)) void
  set_earlier_active_time(Timestamp new_active_time) noexcept;

  void mark_bin_empty(std::size_t linear_bin_index) noexcept {
    nonempty_bins_.reset(linear_bin_index);
    if (linear_bin_index == first_nonempty_bin_) {
      first_nonempty_bin_ = nonempty_bins_.countr_zero();
    }
  }

  void mark_bin_nonempty(std::size_t linear_bin_index) noexcept {
    nonempty_bins_.set(linear_bin_index);
    first_nonempty_bin_ = std::min(
        static_cast<std::uint32_t>(linear_bin_index), first_nonempty_bin_);
  }

  // The base-time of the first bin.
  // All items in the queue will have times that are not before this time.
  // This value will be a multiple of minor_bins_per_major_bin.
  Timestamp active_time_;

  GetTimestampFunc get_timestamp_;

  std::uint32_t first_nonempty_bin_{total_bin_count};

  // A bit set in this bitset indicates the corresponding entry in bins_ is
  // non-empty.
  bitset<total_bin_count> nonempty_bins_;

  // Item list for each bin.
  std::array<item_list, total_bin_count> bins_{};
};

template <
    typename Item,
    Item* Item::* Next,
    Item* Item::* Prev,
    std::unsigned_integral Timestamp,
    std::move_constructible GetTimestampFunc,
    Timestamp MaxTimestamp,
    std::uint32_t MinorBinsPerMajorBin>
  requires std::is_nothrow_invocable_r_v<
               Timestamp,
               const GetTimestampFunc&,
               const Item*>
binned_time_priority_queue<
    Item,
    Next,
    Prev,
    Timestamp,
    GetTimestampFunc,
    MaxTimestamp,
    MinorBinsPerMajorBin>::
    binned_time_priority_queue(GetTimestampFunc get_timestamp) noexcept(
        std::is_nothrow_move_constructible_v<GetTimestampFunc>)
  : active_time_(0)
  , get_timestamp_(std::move(get_timestamp)) {
}

template <
    typename Item,
    Item* Item::* Next,
    Item* Item::* Prev,
    std::unsigned_integral Timestamp,
    std::move_constructible GetTimestampFunc,
    Timestamp MaxTimestamp,
    std::uint32_t MinorBinsPerMajorBin>
  requires std::
      is_nothrow_invocable_r_v<Timestamp, const GetTimestampFunc&, const Item*>
    Timestamp binned_time_priority_queue<
        Item,
        Next,
        Prev,
        Timestamp,
        GetTimestampFunc,
        MaxTimestamp,
        MinorBinsPerMajorBin>::earliest_due_time_lower_bound() const noexcept {
  if (first_nonempty_bin_ == total_bin_count) {
    return MaxTimestamp;
  }

  return get_bin_time(bin_index::from_linear_index(first_nonempty_bin_));
}

template <
    typename Item,
    Item* Item::* Next,
    Item* Item::* Prev,
    std::unsigned_integral Timestamp,
    std::move_constructible GetTimestampFunc,
    Timestamp MaxTimestamp,
    std::uint32_t MinorBinsPerMajorBin>
  requires std::
      is_nothrow_invocable_r_v<Timestamp, const GetTimestampFunc&, const Item*>
    __attribute__((noinline)) void binned_time_priority_queue<
        Item,
        Next,
        Prev,
        Timestamp,
        GetTimestampFunc,
        MaxTimestamp,
        MinorBinsPerMajorBin>::add(Item* item) noexcept {
  const Timestamp item_time = get_item_due_time(item);

  if (item_time < active_time_) {
    set_earlier_active_time(item_time);
  }

  auto bin_index = get_bin_index(item_time);
  auto& bin = get_bin(bin_index);
  if (bin.empty()) {
    mark_bin_nonempty(bin_index.linear_index());
  }
  bin.push_back(item);
}

template <
    typename Item,
    Item* Item::* Next,
    Item* Item::* Prev,
    std::unsigned_integral Timestamp,
    std::move_constructible GetTimestampFunc,
    Timestamp MaxTimestamp,
    std::uint32_t MinorBinsPerMajorBin>
  requires std::
      is_nothrow_invocable_r_v<Timestamp, const GetTimestampFunc&, const Item*>
    // clang-format off
__attribute__((noinline)) void binned_time_priority_queue<
    Item,
    Next,
    Prev,
    Timestamp,
    GetTimestampFunc,
    MaxTimestamp,
    MinorBinsPerMajorBin>::set_earlier_active_time(Timestamp new_active_time) noexcept {
  // clang-format on
  assert(new_active_time < active_time_);

  new_active_time &= active_time_mask;

  // active_time_  -> new_active_time
  // 0xAABBCC00    -> 0xAABBDD00
  //
  //  0xAABBCCxx 0xAABBxx?? 0xAAxx???? 0xxx??????
  // |..........|..........|..........|..........|
  //     |         [0xCC]
  //     |          A
  //      `---------'
  //
  // active_time_  -> new_active_time
  // 0xAABBCC00    -> 0xA9FFFE00
  //
  //  0xAABBCCxx 0xAABBxx?? 0xAAxx???? 0xxx??????
  // |..........|..........|..........|..........|
  //     |            |        |        [0xAA]
  //     |            |        |          A
  //      `------------`--------`---------'

  // First compute the bin that we need to relocate all items to.
  const Timestamp highest_differing_bit =
      std::bit_width(new_active_time ^ active_time_);
  assert(highest_differing_bit > bits_per_major_bin);

  const major_bin_index_t target_major_bin =
      highest_differing_bit / bits_per_major_bin;
  const minor_bin_index_t target_minor_bin =
      (active_time_ >> (target_major_bin * bits_per_major_bin)) &
      minor_index_mask;

  const bin_index target_index{target_major_bin, target_minor_bin};
  const auto target_linear_index = target_index.linear_index();
  auto& target_bin = get_bin(bin_index{target_major_bin, target_minor_bin});
  assert(target_bin.empty());

  // Then move all items in lower major bins into that target bin.
  while (first_nonempty_bin_ < target_linear_index) {
    auto& source_bin = bins_[first_nonempty_bin_];
    target_bin.append(std::move(source_bin));
    nonempty_bins_.reset(first_nonempty_bin_);
    first_nonempty_bin_ = nonempty_bins_.countr_zero();
  }

  active_time_ = new_active_time;
}

template <
    typename Item,
    Item* Item::* Next,
    Item* Item::* Prev,
    std::unsigned_integral Timestamp,
    std::move_constructible GetTimestampFunc,
    Timestamp MaxTimestamp,
    std::uint32_t MinorBinsPerMajorBin>
  requires std::
      is_nothrow_invocable_r_v<Timestamp, const GetTimestampFunc&, const Item*>
    void binned_time_priority_queue<
        Item,
        Next,
        Prev,
        Timestamp,
        GetTimestampFunc,
        MaxTimestamp,
        MinorBinsPerMajorBin>::remove(Item* x) noexcept {
  auto index = get_bin_index(x);
  auto& bin = get_bin(index);
  bin.remove(x);
  if (bin.empty()) {
    mark_bin_empty(index.linear_index());
  }
}

template <
    typename Item,
    Item* Item::* Next,
    Item* Item::* Prev,
    std::unsigned_integral Timestamp,
    std::move_constructible GetTimestampFunc,
    Timestamp MaxTimestamp,
    std::uint32_t MinorBinsPerMajorBin>
  requires std::
      is_nothrow_invocable_r_v<Timestamp, const GetTimestampFunc&, const Item*>
    __attribute__((noinline)) Item* binned_time_priority_queue<
        Item,
        Next,
        Prev,
        Timestamp,
        GetTimestampFunc,
        MaxTimestamp,
        MinorBinsPerMajorBin>::
        try_dequeue_next_due_by(Timestamp current_time) noexcept {
  if (first_nonempty_bin_ == nonempty_bins_.size()) {
    // queue is empty
    return nullptr;
  }

  assert(!bins_[first_nonempty_bin_].empty());

  bin_index index = bin_index::from_linear_index(first_nonempty_bin_);

  Timestamp bin_time = get_bin_time(index);
  if (current_time < bin_time) {
    // Earliest non-empty bin has all elements that are later than
    // 'current_time'.
    return nullptr;
  }

  // If the earliest non-empty bin is not in the first major bin
  // then we need to redistribute the items from the first non-empty
  // bin across the lower-level major bins.
  if (index.major > 0) {
    bin_index source_index = index;
    while (source_index.major != 0) {
      const std::uint32_t source_shift =
          source_index.major * bits_per_major_bin;

      auto& source_bin = get_bin(source_index);
      assert(!source_bin.empty());

      // Update active_time_ with the new base
      const Timestamp active_time_mask =
          (~static_cast<Timestamp>(minor_index_mask) << source_shift);
      active_time_ = (active_time_ & active_time_mask) |
          (static_cast<Timestamp>(source_index.minor) << source_shift);

      const major_bin_index_t target_major = source_index.major - 1;
      const std::uint32_t target_shift = target_major * bits_per_major_bin;
      const std::size_t target_bin_base =
          target_major * minor_bins_per_major_bin;

      minor_bin_index_t first_nonempty_minor_index = minor_bins_per_major_bin;

      while (!source_bin.empty()) {
        Item* const item = source_bin.pop_front();
        const Timestamp item_time = get_item_due_time(item);
        const minor_bin_index_t target_minor_index =
            (item_time >> target_shift) & minor_index_mask;
        const std::size_t target_linear_index =
            target_bin_base + target_minor_index;

        auto& target_bin = bins_[target_linear_index];
        if (target_bin.empty()) {
          nonempty_bins_.set(target_linear_index);
        }

        target_bin.push_back(item);

        if (target_minor_index < first_nonempty_minor_index) {
          first_nonempty_minor_index = target_minor_index;
          first_nonempty_bin_ = target_linear_index;
        }
      }

      nonempty_bins_.reset(source_index.linear_index());

      bin_time = active_time_ + (first_nonempty_minor_index << target_shift);
      if (current_time < bin_time) {
        return nullptr;
      }

      source_index = bin_index{target_major, first_nonempty_minor_index};
    }
    assert(first_nonempty_bin_ < minor_bins_per_major_bin);
  }

  auto& bin = bins_[first_nonempty_bin_];
  assert(!bin.empty());

  Item* item = bin.pop_front();
  if (bin.empty()) {
    mark_bin_empty(first_nonempty_bin_);
  }

  return item;
}

}  // namespace squiz::detail
