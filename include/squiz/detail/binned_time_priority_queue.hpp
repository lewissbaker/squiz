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
/// Uses a hierarchical hashing data-structure that has a number of 'major bins'
/// with the first major bin containing minor bins that span a single time
/// slot and subsequent major bins having minor bins representing a time span
/// that is @c MinorBinsPerMajorBin times the size of the previous major span's
/// minor bins.
/// i.e. a major bin at level N covers the time-span of a single minor bin
/// at level N+1.
///
/// The data-structure keeps track of the 'active_time' which is the base-time
/// of the first major-bin. A minor bin with index 'M' in the first major bin
/// corresponds to items scheduled for time 'active_time + M'.
///
/// For example, say we have a queue with the following parameters:
/// - Timestamp=std::uint16_t
/// - MinorBinsPerMajorBin=16
/// - MaxTimestamp=65535.
///
/// Then if the queue has an 'active_time' value of 0x37B0 then the bins of
/// the queue will correspond to time periods as follows.
///
///      Major 0         Major 1         Major 2        Major 3
/// ||   0x37B?     ||   0x37?*     ||   0x3?**    ||   0x?***     ||
/// ++--------------++--------------++-------------++--------------++
/// ||0|1|2|3| .. |F||0|1|2|3| .. |F||0|1|2| ... |F||0|1|2| ...|E|F||
/// ++--------------++--------------++-------------++--------------++
///   |     |         |     |      |    |         |    |        |
/// 0x37B0  |       0x370*  |   0x37F*  |      0x3F**  |      0xE***
///      0x37B3           0x373*      0x31**         0x1***
///
/// Minor bins that correspond to time buckets with a starting time that is
/// less than the 'active_time' will always be empty.
///
/// Each minor bin has a linked-list of items that belong to the time-span.
///
/// To make it easy to find the earliest non-empty bin, which will contain
/// the item to be dequeued next, the data-structure also maintains a bitset
/// with a single bit for each bin which indicates whether there are items
/// in that bin or not. This allows quickly scanning the bins to find the
/// first non-empty bin.
///
/// A cache of the result of a scan of the bitset is stored in a local variable,
/// so we only need to do the scan for the next earliest bin when the last item
/// is removed from the current earliest bin.
///
/// During a dequeue operation, if the 'Major 0' bin is empty, then we look in
/// 'Major 1' for the next non-empty bin and advance 'active_time' to the time
/// for that bin and then redistribute the items in the next 'Major 1' bin to
/// the corresponding minor bins in the 'Major 0'. If all of the 'Major 1' bins
/// are empty, then the process recurses to the 'Major 2' bin to distribute the
/// items from the earliest 'Major 2' bin to the corresponding bins in the
/// 'Major 1' bin, and so on up until the last major bin.
///
/// This is effectively performing an incremental radix-sort of the items where
/// the radix has base \c MinorBinsPerMajorBin.
///
/// If an item is inserted into the queue that is before the current
/// 'active_time' then we need to update the 'active_time' to be a time before
/// the newly inserted time. This involves moving items from any bins with a
/// start-time before the new active-time to the first bin in a higher-level
/// with a start-time that is not before the new active-time. This process
/// involves a number of bin-moves that is proportional to how far before the
/// current active_time the new item's due-time is, but is constant-time per-bin
/// (i.e. not proportional to the number of elements). So the cost of inserting
/// such an item is still a "constant" in that performs a finite number of
/// operations bounded by a constant, however it is still a more expensive
/// operation than inserting an item at a time later than the current active
/// time.
///
/// As extra work needs to be done when you insert items earlier than the
/// current time, this data-structure will generally perform better if the vast
/// majority of items are inserted at times in the future. i.e. items have a
/// due-time that is at or after the time you last passed to \c
/// try_dequeue_next_due_by().
///
/// If used in this manner, the data-structure will have constant-time insert,
/// constant-time removal, and amortised constant-time dequeuing of the earliest
/// due item.
///
/// This data-structure is intended to be used as follows:
/// - You add items to the queue as necessary.
/// - You obtain the current time, T.
/// - You then execute in a loop \c try_dequeue_next_due_by(T), processing each
/// item
///   until this returns null (or until you have processed enough items for
///   now).
/// - Then you call \c earliest_due_time_lower_bound() to compute the time that
/// you
///   should sleep until - it is guaranteed that there are no items in the queue
///   with a due-time before this time.
///   This time is invalidated if you insert any items scheduled at an earlier
///   time. In this case, you will need to call \c
///   earliest_due_time_lower_bound() again to compute the new earliest time.
/// - Once this time has elapsed then you can repeat the process.
///
/// This data-structure represents all times as integer values.
/// A \c GetTimestampFunc function-object needs to be provided which can be
/// invoked to compute the integer time-value for each queue-item. This allows
/// the item to store the raw due timestamp, say a \c std::chrono::time_point,
/// but then compute the bin-index by applying an offset and quantisation of the
/// raw time.
///
/// For example, say you wanted each bin to correspond to 10ms because that's
/// the finest granularity of time-scheduling you need. You could write your
/// item and
/// \c GetTimestampFunc as follows.
///
/// \example
/// \begin_code
/// struct timer_item {
///   timer_item* next;
///   timer_item* prev;
///   std::chrono::steady_clock::time_point due_time;
/// };
///
/// // Use this type as the 'GetTimestampFunc' type for the queue.
/// struct get_time_scheduling_bin  {
///   std::chrono::steady_clock::time_point start_time =
///   std::chrono::steady_clock::now(); std::uint32_t operator()(const
///   timer_item* item) const noexcept {
///     auto time_since_start = (item->due_time - start_time);
///     if (time_since_start.count() < 0) {
///       return 0;
///     }
///     using ten_milliseconds = std::chrono::duration<std::uint32_t,
///     std::ratio<1, 100>>; if (time_since_start >= ten_milliseconds::max()) {
///       return ten_milliseconds::max().count();
///     }
///     return std::chrono::ceil<ten_milliseconds>(time_since_start).count();
///   }
/// };
/// \end_code
///
/// This data-structure is not safe to access from multiple threads
/// concurrently. Calls to const member-functions are safe to call concurrently
/// with other calls to const member-functions. It is undefined behaviour to
/// make calls to non-const member functions concurrently with calls to any
/// other member-functions.
///
/// \tparam Item
/// The item type to insert into the queue.
///
/// \tparam Next
/// The member-data pointer to the 'Next' data-member that contains a pointer to
/// the next item in a linked list.
///
/// \tparam Prev
/// The member-data pointer to the 'Prev' data-member that contains a pointer to
/// the previous item in a linked list.
///
/// \tparam Timestamp
/// The type of the integer used to represent a time-point.
/// Each integral value represents a separate bin / time-point at which things
/// can be scheduled.
///
/// \tparam GetTimestampFunc
/// The type of the function-object to invoke to obtain the timestamp of an
/// item. Must satisfy be invocable with a single argument of type const Item*.
///
/// \tparam MaxTimestamp
/// The maximum timestamp value of items that will be stored in this queue.
/// This is used to compute the number of major bins required.
///
/// \tparam MinorBinsPerMajorBin
/// The number of minor bins to allocate for each major bin.
/// Must be a power-of-two. Defaults to 256.
/// Note that generated code will generally be more efficient if the exponent of
/// the power-of-two is itself a power-of-two - as this will result in some
/// divisions used in various computations resolving to bit-shifts and masks.
/// i.e. bin-sizes of 2^2, 2^4, 2^8 or 2^16.
/// The smaller the bin size the more compact the data-structure, but the more
/// redistributions will be needed for items scheduled in the future. e.g. if
/// using 2^4 then times more than 16 time quantums in the future will generally
/// need one redistribution step, and times more than 256 time quantums in the
/// future will generally need two redistrubtion steps.
///
// clang-format off
template <
  typename Item,
  Item* Item::* Next,
  Item* Item::* Prev,
  std::unsigned_integral Timestamp,
  std::regular_invocable<const Item*> GetTimestampFunc,
  Timestamp MaxTimestamp = std::numeric_limits<Timestamp>::max(),
  std::uint32_t MinorBinsPerMajorBin = 256>
  requires std::move_constructible<GetTimestampFunc> &&
           std::is_nothrow_invocable_r_v<Timestamp, const GetTimestampFunc&, const Item*>
class binned_time_priority_queue {
  // clang-format on
  static_assert(MaxTimestamp > 0);
  static_assert(std::has_single_bit(MinorBinsPerMajorBin));

public:
  using item_list = intrusive_list<Item, Next, Prev>;

  /// Initializes a \c binned_time_priority_queue with a default-initialized
  /// \c GetTimestampFunc transform.
  binned_time_priority_queue() noexcept(
      std::is_nothrow_default_constructible_v<GetTimestampFunc> &&
      std::is_nothrow_move_constructible_v<GetTimestampFunc>)
    requires std::default_initializable<GetTimestampFunc>
    : binned_time_priority_queue(GetTimestampFunc()) {}

  /// Initializes a \c binned_time_priority_queue with a user-provided
  /// \c GetTimestampFunc transform.
  explicit binned_time_priority_queue(GetTimestampFunc get_timestamp) noexcept(
      std::is_nothrow_move_constructible_v<GetTimestampFunc>);

  /// Move-construct all items and state from one priority queue object to
  /// another.
  binned_time_priority_queue(binned_time_priority_queue&& other) noexcept(
      std::is_nothrow_move_constructible_v<GetTimestampFunc>);

  /// Destroy the queue.
  ///
  /// \pre
  /// \c empty() is \c true.
  ~binned_time_priority_queue() = default;

  /// Query the maximum timestamp value of an item that may be added to the
  /// queue.
  static constexpr Timestamp max_timestamp() noexcept { return MaxTimestamp; }

  /// Query if the queue is empty.
  bool empty() const noexcept {
    return first_nonempty_bin_ == nonempty_bins_.size();
  }

  /// Compute a timestamp such that no items in the queue have a due-time before
  /// this time.
  ///
  /// Note that the result may be approximate in that for due-times further into
  /// the future (i.e. not represented by bins in the first major bin) the
  /// returned due time will potentially be earlier than the actual due-time of
  /// an item by an amount equal to the granularity of the minor min size in
  /// which the next item lives. As the current time advances and due-times get
  /// closer to the last time passed to \c try_dequeue_next_due_by(), the
  /// \c Timestamp returned by this method gets more precise.
  ///
  /// If there are no items in the queue then returns \c MaxTimestamp.
  ///
  /// The \c try_dequeue_next_due_by() function will not return any items until
  /// the current time has at least advanced beyond the returned time.
  Timestamp earliest_due_time_lower_bound() const noexcept;

  /// Add an item to the queue.
  ///
  /// \param x
  /// Pointer to the item to add to the queue.
  /// As this queue is an intrusive linked-list data-structure, it is up to the
  /// caller to ensure that this item remains alive until it is removed or
  /// dequeued.
  ///
  /// The invocation of \c GetTimestampFunc for this item must return a value
  /// that is in the range [0, MaxTimestamp].
  ///
  /// \note
  /// This operation completes in constant-time relative to the number of items
  /// in the queue.
  void add(Item* x) noexcept;

  /// Remove an item from the queue.
  ///
  /// \param x
  /// Pointer to the item to remove from the queue.
  ///
  ///
  /// \note
  /// This operation completes in constant-time.
  void remove(Item* x) noexcept;

  /// Try to dequeue the earliest due item from the queue.
  ///
  /// If there are no items in the queue for which their due timestamp is less
  /// than or equal to \c current_time then returns \c nullptr.
  ///
  /// \param current_time
  /// The maximum timestamp of items to consider for dequeueing.
  ///
  /// \return
  /// A pointer to an item with the earliest due-time.
  /// If there are multiple items with the same earliest due-time then it
  /// returns the item that was added first. If there are no items in the queue
  /// with a due-time at or before the
  /// \c current_time then returns \c nullptr.
  ///
  /// \note
  /// This operation has amortised constant time in the number of items in the
  /// queue, provided that new items are only ever inserted at time-points with
  /// due-times in the future. Certain invocations may require reprocessing a
  /// subset of the items in the queue, but each item will be processed at most
  /// a constant number of times, provided the above usage constraint is
  /// honoured.
  Item* try_dequeue_next_due_by(Timestamp current_time) noexcept;

  /// Reindex all items currently in the queue.
  ///
  /// Call this whenever something changes that invalidates the computation
  /// of the due-time for items in the queue.
  void reindex_items() noexcept;

private:
  static constexpr std::uint32_t minor_bins_per_major_bin =
      MinorBinsPerMajorBin;

  // Note: Passing \c minor_bins_per_major_bin instead of \c
  // minor_bins_per_major_bin-1 as we want to be able to use the value \c
  // minor_bins_per_major_bin to represent an invalid minor bin index.
  using minor_bin_index_t =
      smallest_unsigned_integer_for<minor_bins_per_major_bin>;

  using major_bin_index_t = std::uint32_t;

  static constexpr minor_bin_index_t minor_index_mask =
      (minor_bins_per_major_bin - 1);

  static constexpr Timestamp active_time_mask =
      ~static_cast<Timestamp>(minor_index_mask);

  // How many bits of the timestamp correspond to each major bin.
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
    std::regular_invocable<const Item*> GetTimestampFunc,
    Timestamp MaxTimestamp,
    std::uint32_t MinorBinsPerMajorBin>
  requires std::move_constructible<GetTimestampFunc> &&
               std::is_nothrow_invocable_r_v<
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
    std::regular_invocable<const Item*> GetTimestampFunc,
    Timestamp MaxTimestamp,
    std::uint32_t MinorBinsPerMajorBin>
  requires std::move_constructible<GetTimestampFunc> &&
               std::is_nothrow_invocable_r_v<
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
    binned_time_priority_queue(binned_time_priority_queue&& other) noexcept(
        std::is_nothrow_move_constructible_v<GetTimestampFunc>)
  : active_time_(other.active_time_)
  , get_timestamp_(std::move(other.get_timestamp_))
  , first_nonempty_bin_(other.first_nonempty_bin_)
  , nonempty_bins_(other.nonempty_bins_) {
  for (std::size_t i = 0; i < bins_.size(); ++i) {
    bins_[i].append(std::move(other.bins_[i]));
  }

  other.active_time_ = 0;
  other.nonempty_bins_.reset();
  other.first_nonempty_bin_ = other.nonempty_bins_.size();
}

template <
    typename Item,
    Item* Item::* Next,
    Item* Item::* Prev,
    std::unsigned_integral Timestamp,
    std::regular_invocable<const Item*> GetTimestampFunc,
    Timestamp MaxTimestamp,
    std::uint32_t MinorBinsPerMajorBin>
  requires std::move_constructible<GetTimestampFunc> &&
    std::is_nothrow_invocable_r_v<
               Timestamp,
               const GetTimestampFunc&,
               const Item*>
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
    std::regular_invocable<const Item*> GetTimestampFunc,
    Timestamp MaxTimestamp,
    std::uint32_t MinorBinsPerMajorBin>
  requires std::move_constructible<GetTimestampFunc> &&
    std::is_nothrow_invocable_r_v<
               Timestamp,
               const GetTimestampFunc&,
               const Item*>
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
    std::regular_invocable<const Item*> GetTimestampFunc,
    Timestamp MaxTimestamp,
    std::uint32_t MinorBinsPerMajorBin>
  requires std::move_constructible<GetTimestampFunc> &&
    std::is_nothrow_invocable_r_v<
               Timestamp,
               const GetTimestampFunc&,
               const Item*>
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
    std::regular_invocable<const Item*> GetTimestampFunc,
    Timestamp MaxTimestamp,
    std::uint32_t MinorBinsPerMajorBin>
  requires std::move_constructible<GetTimestampFunc> &&
    std::is_nothrow_invocable_r_v<
               Timestamp,
               const GetTimestampFunc&,
               const Item*>
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
    std::regular_invocable<const Item*> GetTimestampFunc,
    Timestamp MaxTimestamp,
    std::uint32_t MinorBinsPerMajorBin>
  requires std::move_constructible<GetTimestampFunc> &&
    std::is_nothrow_invocable_r_v<
               Timestamp,
               const GetTimestampFunc&,
               const Item*>
__attribute__((noinline)) Item* binned_time_priority_queue<
    Item,
    Next,
    Prev,
    Timestamp,
    GetTimestampFunc,
    MaxTimestamp,
    MinorBinsPerMajorBin>::try_dequeue_next_due_by(Timestamp
                                                       current_time) noexcept {
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

  assert(get_item_due_time(item) <= current_time);

  return item;
}

template <
    typename Item,
    Item* Item::* Next,
    Item* Item::* Prev,
    std::unsigned_integral Timestamp,
    std::regular_invocable<const Item*> GetTimestampFunc,
    Timestamp MaxTimestamp,
    std::uint32_t MinorBinsPerMajorBin>
  requires std::move_constructible<GetTimestampFunc> &&
    std::is_nothrow_invocable_r_v<
               Timestamp,
               const GetTimestampFunc&,
               const Item*>
__attribute__((noinline)) void binned_time_priority_queue<
    Item,
    Next,
    Prev,
    Timestamp,
    GetTimestampFunc,
    MaxTimestamp,
    MinorBinsPerMajorBin>::reindex_items() noexcept {
  intrusive_list<Item, Next, Prev> all_items;

  while (first_nonempty_bin_ != total_bin_count) {
    auto& bin = bins_[first_nonempty_bin_];
    all_items.append(std::move(bin));
    nonempty_bins_.set(first_nonempty_bin_, false);
    first_nonempty_bin_ = nonempty_bins_.countr_zero();
  }

  active_time_ = 0;

  while (!all_items.empty()) {
    add(all_items.pop_front());
  }
}

}  // namespace squiz::detail
