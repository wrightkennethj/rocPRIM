// Copyright (c) 2017 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#ifndef ROCPRIM_BLOCK_BLOCK_RADIX_SORT_HPP_
#define ROCPRIM_BLOCK_BLOCK_RADIX_SORT_HPP_

#include <type_traits>

#include "../config.hpp"
#include "../detail/various.hpp"
#include "../detail/radix_sort.hpp"
#include "../warp/detail/warp_scan_shuffle.hpp"

#include "../intrinsics.hpp"
#include "../functional.hpp"
#include "../types.hpp"

#include "block_exchange.hpp"

/// \addtogroup collectiveblockmodule
/// @{

BEGIN_ROCPRIM_NAMESPACE

namespace detail
{

/// Allows to reduce and scan multimple sequences simultaneously
template<class T, unsigned int Size>
struct buckets
{
    static constexpr unsigned int size = Size;
    T xs[Size];

    ROCPRIM_DEVICE inline
    buckets()
    {
        for(unsigned int r = 0; r < Size; r++)
        {
            xs[r] = T(0);
        }
    }

    ROCPRIM_DEVICE inline
    buckets operator+(const buckets& b) const
    {
        buckets c;
        for(unsigned int r = 0; r < Size; r++)
        {
            c[r] = xs[r] + b[r];
        }
        return c;
    }

    ROCPRIM_DEVICE inline
    T& operator[](unsigned int idx)
    {
        return xs[idx];
    }

    ROCPRIM_DEVICE inline
    T operator[](unsigned int idx) const
    {
        return xs[idx];
    }
};

/// Specialized warp scan and reduce functions of bool (1 bit values)
/// They have much better performance (several times faster) than generic scan and reduce classes
/// because of using hardware ability to calculate which lanes have true predicate values.

template<class T>
ROCPRIM_DEVICE inline
void warp_bit_plus_exclusive_scan(const T input, T& output)
{
    output = ::rocprim::masked_bit_count(::rocprim::ballot(input));
}

template<class T, unsigned int Size>
ROCPRIM_DEVICE inline
void warp_bit_plus_exclusive_scan(const buckets<T, Size>& input, buckets<T, Size>& output)
{
    for(unsigned int r = 0; r < Size; r++)
    {
        warp_bit_plus_exclusive_scan(input[r], output[r]);
    }
}

template<class T>
ROCPRIM_DEVICE inline
void warp_bit_plus_reduce(const T input, T& output)
{
    output = ::rocprim::bit_count(::rocprim::ballot(input));
}

template<class T, unsigned int Size>
ROCPRIM_DEVICE inline
void warp_bit_plus_reduce(const buckets<T, Size>& input, buckets<T, Size>& output)
{
    for(unsigned int r = 0; r < Size; r++)
    {
        warp_bit_plus_reduce(input[r], output[r]);
    }
}

/// Specialized block scan of bool (1 bit values)
template<
    class T,
    unsigned int BlockSize
>
class block_bit_plus_scan
{
    // Select warp size
    static constexpr unsigned int warp_size =
        detail::get_min_warp_size(BlockSize, ::rocprim::warp_size());
    // Number of warps in block
    static constexpr unsigned int warps_no = (BlockSize + warp_size - 1) / warp_size;

    // typedef of warp_scan primitive that will be used to get prefix values for
    // each warp (scanned carry-outs from warps before it)
    // warp_scan_shuffle is an implementation of warp_scan that does not need storage,
    // but requires logical warp size to be a power of two.
    using warp_scan_prefix_type = ::rocprim::detail::warp_scan_shuffle<T, detail::next_power_of_two(warps_no)>;

public:

    struct storage_type
    {
        T warp_prefixes[warps_no];
        // ---------- Shared memory optimisation ----------
        // Since we use warp_scan_shuffle for warp scan, we don't need to allocate
        // any temporary memory for it.
    };

    template<unsigned int ItemsPerThread>
    ROCPRIM_DEVICE inline
    void exclusive_scan(const T (&input)[ItemsPerThread],
                        T (&output)[ItemsPerThread],
                        T& reduction,
                        storage_type& storage)
    {
        const unsigned int lane_id = ::rocprim::lane_id();
        const unsigned int warp_id = ::rocprim::warp_id();

        T warp_reduction;
        warp_bit_plus_reduce(input[0], warp_reduction);
        for(unsigned int i = 1; i < ItemsPerThread; i++)
        {
            T r;
            warp_bit_plus_reduce(input[i], r);
            warp_reduction = warp_reduction + r;
        }
        if(lane_id == 0)
        {
            storage.warp_prefixes[warp_id] = warp_reduction;
        }
        ::rocprim::syncthreads();

        // Scan the warp reduction results to calculate warp prefixes
        if(warp_id == 0)
        {
            if(lane_id < warps_no)
            {
                T prefix = storage.warp_prefixes[lane_id];
                warp_scan_prefix_type().inclusive_scan(prefix, prefix, ::rocprim::plus<T>());
                storage.warp_prefixes[lane_id] = prefix;
            }
        }
        ::rocprim::syncthreads();

        // Perform exclusive warp scan of bit values
        T lane_prefix;
        warp_bit_plus_exclusive_scan(input[0], lane_prefix);
        for(unsigned int i = 1; i < ItemsPerThread; i++)
        {
            T s;
            warp_bit_plus_exclusive_scan(input[i], s);
            lane_prefix = lane_prefix + s;
        }

        // Scan the lane's items and calculate final scan results
        output[0] = warp_id == 0
            ? lane_prefix
            : lane_prefix + storage.warp_prefixes[warp_id - 1];
        for(unsigned int i = 1; i < ItemsPerThread; i++)
        {
            output[i] = output[i - 1] + input[i - 1];
        }

        // Get the final inclusive reduction result
        reduction = storage.warp_prefixes[warps_no - 1];
    }
};

} // end namespace detail

/// \brief The block_radix_sort class is a block level parallel primitive which provides
/// methods sorting items (keys or key-value pairs) partitioned across threads in a block
/// using radix sort algorithm.
///
/// \tparam Key - the key type.
/// \tparam BlockSize - the number of threads in a block.
/// \tparam ItemsPerThread - the number of items contributed by each thread.
/// \tparam Value - the value type. Default type empty_type indicates
/// a keys-only sort.
/// \tparam RadixBits - The number of radix bits.
///
/// \par Overview
/// * \p Key type must be an arithmetic type (that is, an integral type or a floating-point
/// type).
/// * Performance depends on \p BlockSize, \p ItemsPerThread and \p RadixBits.
///   * It is usually better of \p BlockSize is a multiple of the size of the hardware warp.
///   * It is usually increased when \p ItemsPerThread is greater than one. However, when there
///   are too many items per thread, each thread may need so much registers and/or shared memory
///   that occupancy will fall too low, decreasing the performance.
///   * If \p Key is an integer type and the range of keys is known in advance, the performance
///   can be improved by setting \p begin_bit and \p end_bit, for example if all keys are in range
///   [100, 10000], <tt>begin_bit = 5</tt> and <tt>end_bit = 14</tt> will cover the whole range.
///
/// \par Examples
/// \parblock
/// In the examples radix sort is performed on a block of 256 threads, each thread provides
/// eight \p int value, results are returned using the same array as for input.
///
/// \b HIP: \n
/// \code{.cpp}
/// __global__ void example_kernel(...)
/// {
///     // specialize block_radix_sort for int, block of 256 threads,
///     // and eight items per thread; key-only sort
///     using block_rsort_int = rocprim::block_radix_sort<int, 256, 8>;
///     // allocate storage in shared memory
///     __shared__ block_rsort_int::storage_type storage;
///
///     int input[8] = ...;
///     // execute block radix sort (ascending)
///     block_rsort_int().sort(
///         input,
///         storage
///     );
///     ...
/// }
/// \endcode
///
/// \b HC: \n
/// \code{.cpp}
/// hc::parallel_for_each(
///     hc::extent<1>(...).tile(256),
///     [=](hc::tiled_index<1> i) [[hc]]
///     {
///         // specialize block_radix_sort for int, block of 256 threads,
///         // and eight items per thread; key-only sort
///         using block_rsort_int = rocprim::block_radix_sort<int, 256, 8>;
///
///         // allocate storage in shared memory
///         tile_static block_rsort_int::storage_type storage;
///
///         int input[8] = ...;
///         // execute block radix sort (ascending)
///         block_rsort_int().sort(
///             input,
///             storage
///         );
///     }
/// );
/// \endcode
/// \endparblock
template<
    class Key,
    unsigned int BlockSize,
    unsigned int ItemsPerThread = 1,
    class Value = empty_type,
    unsigned int RadixBits = 1
>
class block_radix_sort
{
    static constexpr bool with_values = !std::is_same<Value, empty_type>::value;
    static constexpr unsigned int radix_size = 1 << RadixBits;

    using bit_key_type = typename ::rocprim::detail::radix_key_codec<Key>::bit_key_type;

    // The last radix value does not have its own bucket and hence no scan is performed, because
    // its value can be calculated based on all other values
    using buckets = detail::buckets<unsigned int, radix_size - 1>;
    using bit_block_scan = detail::block_bit_plus_scan<buckets, BlockSize>;

    using bit_keys_exchange_type = ::rocprim::block_exchange<bit_key_type, BlockSize, ItemsPerThread>;
    using values_exchange_type = ::rocprim::block_exchange<Value, BlockSize, ItemsPerThread>;

public:

    /// \brief Struct used to allocate a temporary memory that is required for thread
    /// communication during operations provided by related parallel primitive.
    ///
    /// Depending on the implemention the operations exposed by parallel primitive may
    /// require a temporary storage for thread communication. The storage should be allocated
    /// using keywords <tt>__shared__</tt> in HIP or \p tile_static in HC. It can be aliased to
    /// an externally allocated memory, or be a part of a union type with other storage types
    /// to increase shared memory reusability.
    #ifndef DOXYGEN_SHOULD_SKIP_THIS // hides storage_type implementation for Doxygen
    struct storage_type
    {
        union
        {
            typename bit_keys_exchange_type::storage_type bit_keys_exchange;
            typename values_exchange_type::storage_type values_exchange;
        };
        typename bit_block_scan::storage_type bit_block_scan;
    };
    #else
    using storage_type = storage_type_; // only for Doxygen
    #endif

    /// \brief Performs ascending radix sort over keys partitioned across threads in a block.
    ///
    /// \param [in, out] keys - reference to an array of keys provided by a thread.
    /// \param [in] storage - reference to a temporary storage object of type storage_type.
    /// \param [in] begin_bit - [optional] index of the first (least significant) bit used in
    /// key comparison. Must be in range <tt>[0; 8 * sizeof(Key))</tt>. Default value: \p 0.
    /// \param [in] end_bit - [optional] past-the-end index (most significant) bit used in
    /// key comparison. Must be in range <tt>(begin_bit; 8 * sizeof(Key)]</tt>. Default
    /// value: \p <tt>8 * sizeof(Key)</tt>.
    ///
    /// \par Storage reusage
    /// Synchronization barrier should be placed before \p storage is reused
    /// or repurposed: \p __syncthreads() in HIP, \p tile_barrier::wait() in HC, or
    /// universal rocprim::syncthreads().
    ///
    /// \par Examples
    /// \parblock
    /// In the examples radix sort is performed on a block of 128 threads, each thread provides
    /// two \p float value, results are returned using the same array as for input.
    ///
    /// \b HIP: \n
    /// \code{.cpp}
    /// __global__ void example_kernel(...)
    /// {
    ///     // specialize block_radix_sort for float, block of 128 threads,
    ///     // and two items per thread; key-only sort
    ///     using block_rsort_float = rocprim::block_radix_sort<float, 128, 2>;
    ///     // allocate storage in shared memory
    ///     __shared__ block_rsort_float::storage_type storage;
    ///
    ///     float input[2] = ...;
    ///     // execute block radix sort (ascending)
    ///     block_rsort_float().sort(
    ///         input,
    ///         storage
    ///     );
    ///     ...
    /// }
    /// \endcode
    ///
    /// \b HC: \n
    /// \code{.cpp}
    /// hc::parallel_for_each(
    ///     hc::extent<1>(...).tile(128),
    ///     [=](hc::tiled_index<1> i) [[hc]]
    ///     {
    ///         // specialize block_radix_sort for float, block of 128 threads,
    ///         // and two items per thread; key-only sort
    ///         using block_rsort_float = rocprim::block_radix_sort<float, 128, 2>;
    ///
    ///         // allocate storage in shared memory
    ///         tile_static block_rsort_float::storage_type storage;
    ///
    ///         float input[2] = ...;
    ///         // execute block radix sort (ascending)
    ///         block_rsort_float().sort(
    ///             input,
    ///             storage
    ///         );
    ///     }
    /// );
    /// \endcode
    /// If the \p input values across threads in a block are <tt>{[256, 255], ..., [4, 3], [2, 1]}}</tt>, then
    /// then after sort they will be equal <tt>{[1, 2], [3, 4]  ..., [255, 256]}</tt>.
    /// \endparblock
    ROCPRIM_DEVICE inline
    void sort(Key (&keys)[ItemsPerThread],
              storage_type& storage,
              unsigned int begin_bit = 0,
              unsigned int end_bit = 8 * sizeof(Key))
    {
        empty_type * values = nullptr;
        sort_impl<false>(keys, values, storage, begin_bit, end_bit);
    }

    /// \overload
    /// \brief Performs ascending radix sort over keys partitioned across threads in a block.
    ///
    /// * This overload does not accept storage argument. Required shared memory is
    /// allocated by the method itself.
    ///
    /// \param [in, out] keys - reference to an array of keys provided by a thread.
    /// \param [in] begin_bit - [optional] index of the first (least significant) bit used in
    /// key comparison. Must be in range <tt>[0; 8 * sizeof(Key))</tt>. Default value: \p 0.
    /// \param [in] end_bit - [optional] past-the-end index (most significant) bit used in
    /// key comparison. Must be in range <tt>(begin_bit; 8 * sizeof(Key)]</tt>. Default
    /// value: \p <tt>8 * sizeof(Key)</tt>.
    ROCPRIM_DEVICE inline
    void sort(Key (&keys)[ItemsPerThread],
              unsigned int begin_bit = 0,
              unsigned int end_bit = 8 * sizeof(Key))
    {
        ROCPRIM_SHARED_MEMORY storage_type storage;
        sort(keys, storage, begin_bit, end_bit);
    }

    /// \brief Performs descending radix sort over keys partitioned across threads in a block.
    ///
    /// \param [in, out] keys - reference to an array of keys provided by a thread.
    /// \param [in] storage - reference to a temporary storage object of type storage_type.
    /// \param [in] begin_bit - [optional] index of the first (least significant) bit used in
    /// key comparison. Must be in range <tt>[0; 8 * sizeof(Key))</tt>. Default value: \p 0.
    /// \param [in] end_bit - [optional] past-the-end index (most significant) bit used in
    /// key comparison. Must be in range <tt>(begin_bit; 8 * sizeof(Key)]</tt>. Default
    /// value: \p <tt>8 * sizeof(Key)</tt>.
    ///
    /// \par Storage reusage
    /// Synchronization barrier should be placed before \p storage is reused
    /// or repurposed: \p __syncthreads() in HIP, \p tile_barrier::wait() in HC, or
    /// universal rocprim::syncthreads().
    ///
    /// \par Examples
    /// \parblock
    /// In the examples radix sort is performed on a block of 128 threads, each thread provides
    /// two \p float value, results are returned using the same array as for input.
    ///
    /// \b HIP: \n
    /// \code{.cpp}
    /// __global__ void example_kernel(...)
    /// {
    ///     // specialize block_radix_sort for float, block of 128 threads,
    ///     // and two items per thread; key-only sort
    ///     using block_rsort_float = rocprim::block_radix_sort<float, 128, 2>;
    ///     // allocate storage in shared memory
    ///     __shared__ block_rsort_float::storage_type storage;
    ///
    ///     float input[2] = ...;
    ///     // execute block radix sort (descending)
    ///     block_rsort_float().sort_desc(
    ///         input,
    ///         storage
    ///     );
    ///     ...
    /// }
    /// \endcode
    ///
    /// \b HC: \n
    /// \code{.cpp}
    /// hc::parallel_for_each(
    ///     hc::extent<1>(...).tile(128),
    ///     [=](hc::tiled_index<1> i) [[hc]]
    ///     {
    ///         // specialize block_radix_sort for float, block of 128 threads,
    ///         // and two items per thread; key-only sort
    ///         using block_rsort_float = rocprim::block_radix_sort<float, 128, 2>;
    ///
    ///         // allocate storage in shared memory
    ///         tile_static block_rsort_float::storage_type storage;
    ///
    ///         float input[2] = ...;
    ///         // execute block radix sort (descending)
    ///         block_rsort_float().sort_desc(
    ///             input,
    ///             storage
    ///         );
    ///     }
    /// );
    /// \endcode
    /// If the \p input values across threads in a block are <tt>{[1, 2], [3, 4]  ..., [255, 256]}</tt>,
    /// then after sort they will be equal <tt>{[256, 255], ..., [4, 3], [2, 1]}</tt>.
    /// \endparblock
    ROCPRIM_DEVICE inline
    void sort_desc(Key (&keys)[ItemsPerThread],
                   storage_type& storage,
                   unsigned int begin_bit = 0,
                   unsigned int end_bit = 8 * sizeof(Key))
    {
        empty_type * values = nullptr;
        sort_impl<true>(keys, values, storage, begin_bit, end_bit);
    }

    /// \overload
    /// \brief Performs descending radix sort over keys partitioned across threads in a block.
    ///
    /// * This overload does not accept storage argument. Required shared memory is
    /// allocated by the method itself.
    ///
    /// \param [in, out] keys - reference to an array of keys provided by a thread.
    /// \param [in] begin_bit - [optional] index of the first (least significant) bit used in
    /// key comparison. Must be in range <tt>[0; 8 * sizeof(Key))</tt>. Default value: \p 0.
    /// \param [in] end_bit - [optional] past-the-end index (most significant) bit used in
    /// key comparison. Must be in range <tt>(begin_bit; 8 * sizeof(Key)]</tt>. Default
    /// value: \p <tt>8 * sizeof(Key)</tt>.
    ROCPRIM_DEVICE inline
    void sort_desc(Key (&keys)[ItemsPerThread],
                   unsigned int begin_bit = 0,
                   unsigned int end_bit = 8 * sizeof(Key))
    {
        ROCPRIM_SHARED_MEMORY storage_type storage;
        sort_desc(keys, storage, begin_bit, end_bit);
    }

    /// \brief Performs ascending radix sort over key-value pairs partitioned across
    /// threads in a block.
    ///
    /// \pre Method is enabled only if \p Value type is different than empty_type.
    ///
    /// \param [in, out] keys - reference to an array of keys provided by a thread.
    /// \param [in, out] values - reference to an array of values provided by a thread.
    /// \param [in] storage - reference to a temporary storage object of type storage_type.
    /// \param [in] begin_bit - [optional] index of the first (least significant) bit used in
    /// key comparison. Must be in range <tt>[0; 8 * sizeof(Key))</tt>. Default value: \p 0.
    /// \param [in] end_bit - [optional] past-the-end index (most significant) bit used in
    /// key comparison. Must be in range <tt>(begin_bit; 8 * sizeof(Key)]</tt>. Default
    /// value: \p <tt>8 * sizeof(Key)</tt>.
    ///
    /// \par Storage reusage
    /// Synchronization barrier should be placed before \p storage is reused
    /// or repurposed: \p __syncthreads() in HIP, \p tile_barrier::wait() in HC, or
    /// universal rocprim::syncthreads().
    ///
    /// \par Examples
    /// \parblock
    /// In the examples radix sort is performed on a block of 128 threads, each thread provides
    /// two key-value <tt>int</tt>-<tt>float</tt> pairs, results are returned using the same
    /// arrays as for input.
    ///
    /// \b HIP: \n
    /// \code{.cpp}
    /// __global__ void example_kernel(...)
    /// {
    ///     // specialize block_radix_sort for int-float pairs, block of 128
    ///     // threads, and two items per thread
    ///     using block_rsort_ii = rocprim::block_radix_sort<int, 128, 2, int>;
    ///     // allocate storage in shared memory
    ///     __shared__ block_rsort_ii::storage_type storage;
    ///
    ///     int keys[2] = ...;
    ///     float values[2] = ...;
    ///     // execute block radix sort-by-key (ascending)
    ///     block_rsort_ii().sort(
    ///         keys, values,
    ///         storage
    ///     );
    ///     ...
    /// }
    /// \endcode
    ///
    /// \b HC: \n
    /// \code{.cpp}
    /// hc::parallel_for_each(
    ///     hc::extent<1>(...).tile(128),
    ///     [=](hc::tiled_index<1> i) [[hc]]
    ///     {
    ///         // specialize block_radix_sort for int-float pairs, block of 128
    ///         // threads, and two items per thread
    ///         using block_rsort_ii = rocprim::block_radix_sort<int, 128, 2, int>;
    ///
    ///         // allocate storage in shared memory
    ///         tile_static block_rsort_ii::storage_type storage;
    ///
    ///         int keys[2] = ...;
    ///         float values[2] = ...;
    ///         // execute block radix sort-by-key (ascending)
    ///         block_rsort_ii().sort(
    ///             keys, values,
    ///             storage
    ///         );
    ///         ...
    ///     }
    /// );
    /// \endcode
    /// If the \p keys across threads in a block are <tt>{[256, 255], ..., [4, 3], [2, 1]}</tt> and
    /// the \p values are <tt>{[1, 1], [2, 2]  ..., [128, 128]}</tt>, then after sort the \p keys
    /// will be equal <tt>{[1, 2], [3, 4]  ..., [255, 256]}</tt> and the \p values will be
    /// equal <tt>{[128, 128], [127, 127]  ..., [2, 2], [1, 1]}</tt>.
    /// \endparblock
    template<bool WithValues = with_values>
    ROCPRIM_DEVICE inline
    void sort(Key (&keys)[ItemsPerThread],
              typename std::enable_if<WithValues, Value>::type (&values)[ItemsPerThread],
              storage_type& storage,
              unsigned int begin_bit = 0,
              unsigned int end_bit = 8 * sizeof(Key))
    {
        sort_impl<false>(keys, values, storage, begin_bit, end_bit);
    }

    /// \overload
    /// \brief Performs ascending radix sort over key-value pairs partitioned across
    /// threads in a block.
    ///
    /// * This overload does not accept storage argument. Required shared memory is
    /// allocated by the method itself.
    ///
    /// \pre Method is enabled only if \p Value type is different than empty_type.
    ///
    /// \param [in, out] keys - reference to an array of keys provided by a thread.
    /// \param [in, out] values - reference to an array of values provided by a thread.
    /// \param [in] begin_bit - [optional] index of the first (least significant) bit used in
    /// key comparison. Must be in range <tt>[0; 8 * sizeof(Key))</tt>. Default value: \p 0.
    /// \param [in] end_bit - [optional] past-the-end index (most significant) bit used in
    /// key comparison. Must be in range <tt>(begin_bit; 8 * sizeof(Key)]</tt>. Default
    /// value: \p <tt>8 * sizeof(Key)</tt>.
    template<bool WithValues = with_values>
    ROCPRIM_DEVICE inline
    void sort(Key (&keys)[ItemsPerThread],
              typename std::enable_if<WithValues, Value>::type (&values)[ItemsPerThread],
              unsigned int begin_bit = 0,
              unsigned int end_bit = 8 * sizeof(Key))
    {
        ROCPRIM_SHARED_MEMORY storage_type storage;
        sort(keys, values, storage, begin_bit, end_bit);
    }

    /// \brief Performs descending radix sort over key-value pairs partitioned across
    /// threads in a block.
    ///
    /// \pre Method is enabled only if \p Value type is different than empty_type.
    ///
    /// \param [in, out] keys - reference to an array of keys provided by a thread.
    /// \param [in, out] values - reference to an array of values provided by a thread.
    /// \param [in] storage - reference to a temporary storage object of type storage_type.
    /// \param [in] begin_bit - [optional] index of the first (least significant) bit used in
    /// key comparison. Must be in range <tt>[0; 8 * sizeof(Key))</tt>. Default value: \p 0.
    /// \param [in] end_bit - [optional] past-the-end index (most significant) bit used in
    /// key comparison. Must be in range <tt>(begin_bit; 8 * sizeof(Key)]</tt>. Default
    /// value: \p <tt>8 * sizeof(Key)</tt>.
    ///
    /// \par Storage reusage
    /// Synchronization barrier should be placed before \p storage is reused
    /// or repurposed: \p __syncthreads() in HIP, \p tile_barrier::wait() in HC, or
    /// universal rocprim::syncthreads().
    ///
    /// \par Examples
    /// \parblock
    /// In the examples radix sort is performed on a block of 128 threads, each thread provides
    /// two key-value <tt>int</tt>-<tt>float</tt> pairs, results are returned using the same
    /// arrays as for input.
    ///
    /// \b HIP: \n
    /// \code{.cpp}
    /// __global__ void example_kernel(...)
    /// {
    ///     // specialize block_radix_sort for int-float pairs, block of 128
    ///     // threads, and two items per thread
    ///     using block_rsort_ii = rocprim::block_radix_sort<int, 128, 2, int>;
    ///     // allocate storage in shared memory
    ///     __shared__ block_rsort_ii::storage_type storage;
    ///
    ///     int keys[2] = ...;
    ///     float values[2] = ...;
    ///     // execute block radix sort-by-key (descending)
    ///     block_rsort_ii().sort_desc(
    ///         keys, values,
    ///         storage
    ///     );
    ///     ...
    /// }
    /// \endcode
    ///
    /// \b HC: \n
    /// \code{.cpp}
    /// hc::parallel_for_each(
    ///     hc::extent<1>(...).tile(128),
    ///     [=](hc::tiled_index<1> i) [[hc]]
    ///     {
    ///         // specialize block_radix_sort for int-float pairs, block of 128
    ///         // threads, and two items per thread
    ///         using block_rsort_ii = rocprim::block_radix_sort<int, 128, 2, int>;
    ///
    ///         // allocate storage in shared memory
    ///         tile_static block_rsort_ii::storage_type storage;
    ///
    ///         int keys[2] = ...;
    ///         float values[2] = ...;
    ///         // execute block radix sort-by-key (descending)
    ///         block_rsort_ii().sort_desc(
    ///             keys, values,
    ///             storage
    ///         );
    ///         ...
    ///     }
    /// );
    /// \endcode
    /// If the \p keys across threads in a block are <tt>{[1, 2], [3, 4]  ..., [255, 256]}</tt> and
    /// the \p values are <tt>{[128, 128], [127, 127]  ..., [2, 2], [1, 1]}</tt>, then after sort
    /// the \p keys will be equal <tt>{[256, 255], ..., [4, 3], [2, 1]}</tt> and the \p values
    /// will be equal <tt>{[1, 1], [2, 2]  ..., [128, 128]}</tt>.
    /// \endparblock
    template<bool WithValues = with_values>
    ROCPRIM_DEVICE inline
    void sort_desc(Key (&keys)[ItemsPerThread],
                   typename std::enable_if<WithValues, Value>::type (&values)[ItemsPerThread],
                   storage_type& storage,
                   unsigned int begin_bit = 0,
                   unsigned int end_bit = 8 * sizeof(Key))
    {
        sort_impl<true>(keys, values, storage, begin_bit, end_bit);
    }

    /// \overload
    /// \brief Performs descending radix sort over key-value pairs partitioned across
    /// threads in a block.
    ///
    /// * This overload does not accept storage argument. Required shared memory is
    /// allocated by the method itself.
    ///
    /// \pre Method is enabled only if \p Value type is different than empty_type.
    ///
    /// \param [in, out] keys - reference to an array of keys provided by a thread.
    /// \param [in, out] values - reference to an array of values provided by a thread.
    /// \param [in] begin_bit - [optional] index of the first (least significant) bit used in
    /// key comparison. Must be in range <tt>[0; 8 * sizeof(Key))</tt>. Default value: \p 0.
    /// \param [in] end_bit - [optional] past-the-end index (most significant) bit used in
    /// key comparison. Must be in range <tt>(begin_bit; 8 * sizeof(Key)]</tt>. Default
    /// value: \p <tt>8 * sizeof(Key)</tt>.
    template<bool WithValues = with_values>
    ROCPRIM_DEVICE inline
    void sort_desc(Key (&keys)[ItemsPerThread],
                   typename std::enable_if<WithValues, Value>::type (&values)[ItemsPerThread],
                   unsigned int begin_bit = 0,
                   unsigned int end_bit = 8 * sizeof(Key))
    {
        ROCPRIM_SHARED_MEMORY storage_type storage;
        sort_desc(keys, values, storage, begin_bit, end_bit);
    }

    /// \brief Performs ascending radix sort over keys partitioned across threads in a block,
    /// results are saved in a striped arrangement.
    ///
    /// \param [in, out] keys - reference to an array of keys provided by a thread.
    /// \param [in] storage - reference to a temporary storage object of type storage_type.
    /// \param [in] begin_bit - [optional] index of the first (least significant) bit used in
    /// key comparison. Must be in range <tt>[0; 8 * sizeof(Key))</tt>. Default value: \p 0.
    /// \param [in] end_bit - [optional] past-the-end index (most significant) bit used in
    /// key comparison. Must be in range <tt>(begin_bit; 8 * sizeof(Key)]</tt>. Default
    /// value: \p <tt>8 * sizeof(Key)</tt>.
    ///
    /// \par Storage reusage
    /// Synchronization barrier should be placed before \p storage is reused
    /// or repurposed: \p __syncthreads() in HIP, \p tile_barrier::wait() in HC, or
    /// universal rocprim::syncthreads().
    ///
    /// \par Examples
    /// \parblock
    /// In the examples radix sort is performed on a block of 128 threads, each thread provides
    /// two \p float value, results are returned using the same array as for input.
    ///
    /// \b HIP: \n
    /// \code{.cpp}
    /// __global__ void example_kernel(...)
    /// {
    ///     // specialize block_radix_sort for float, block of 128 threads,
    ///     // and two items per thread; key-only sort
    ///     using block_rsort_float = rocprim::block_radix_sort<float, 128, 2>;
    ///     // allocate storage in shared memory
    ///     __shared__ block_rsort_float::storage_type storage;
    ///
    ///     float keys[2] = ...;
    ///     // execute block radix sort (ascending)
    ///     block_rsort_float().sort_to_striped(
    ///         keys,
    ///         storage
    ///     );
    ///     ...
    /// }
    /// \endcode
    ///
    /// \b HC: \n
    /// \code{.cpp}
    /// hc::parallel_for_each(
    ///     hc::extent<1>(...).tile(128),
    ///     [=](hc::tiled_index<1> i) [[hc]]
    ///     {
    ///         // specialize block_radix_sort for float, block of 128 threads,
    ///         // and two items per thread; key-only sort
    ///         using block_rsort_float = rocprim::block_radix_sort<float, 128, 2>;
    ///
    ///         // allocate storage in shared memory
    ///         tile_static block_rsort_float::storage_type storage;
    ///
    ///         float keys[2] = ...;
    ///         // execute block radix sort (ascending)
    ///         block_rsort_float().sort_to_striped(
    ///             keys,
    ///             storage
    ///         );
    ///     }
    /// );
    /// \endcode
    /// If the \p input values across threads in a block are <tt>{[256, 255], ..., [4, 3], [2, 1]}}</tt>, then
    /// then after sort they will be equal <tt>{[1, 129], [2, 130]  ..., [128, 256]}</tt>.
    /// \endparblock
    ROCPRIM_DEVICE inline
    void sort_to_striped(Key (&keys)[ItemsPerThread],
                         storage_type& storage,
                         unsigned int begin_bit = 0,
                         unsigned int end_bit = 8 * sizeof(Key))
    {
        empty_type * values = nullptr;
        sort_impl<false, true>(keys, values, storage, begin_bit, end_bit);
    }

    /// \overload
    /// \brief Performs ascending radix sort over keys partitioned across threads in a block,
    /// results are saved in a striped arrangement.
    ///
    /// * This overload does not accept storage argument. Required shared memory is
    /// allocated by the method itself.
    ///
    /// \param [in, out] keys - reference to an array of keys provided by a thread.
    /// \param [in] begin_bit - [optional] index of the first (least significant) bit used in
    /// key comparison. Must be in range <tt>[0; 8 * sizeof(Key))</tt>. Default value: \p 0.
    /// \param [in] end_bit - [optional] past-the-end index (most significant) bit used in
    /// key comparison. Must be in range <tt>(begin_bit; 8 * sizeof(Key)]</tt>. Default
    /// value: \p <tt>8 * sizeof(Key)</tt>.
    ROCPRIM_DEVICE inline
    void sort_to_striped(Key (&keys)[ItemsPerThread],
                         unsigned int begin_bit = 0,
                         unsigned int end_bit = 8 * sizeof(Key))
    {
        ROCPRIM_SHARED_MEMORY storage_type storage;
        sort_to_striped(keys, storage, begin_bit, end_bit);
    }

    /// \brief Performs descending radix sort over keys partitioned across threads in a block,
    /// results are saved in a striped arrangement.
    ///
    /// \param [in, out] keys - reference to an array of keys provided by a thread.
    /// \param [in] storage - reference to a temporary storage object of type storage_type.
    /// \param [in] begin_bit - [optional] index of the first (least significant) bit used in
    /// key comparison. Must be in range <tt>[0; 8 * sizeof(Key))</tt>. Default value: \p 0.
    /// \param [in] end_bit - [optional] past-the-end index (most significant) bit used in
    /// key comparison. Must be in range <tt>(begin_bit; 8 * sizeof(Key)]</tt>. Default
    /// value: \p <tt>8 * sizeof(Key)</tt>.
    ///
    /// \par Storage reusage
    /// Synchronization barrier should be placed before \p storage is reused
    /// or repurposed: \p __syncthreads() in HIP, \p tile_barrier::wait() in HC, or
    /// universal rocprim::syncthreads().
    ///
    /// \par Examples
    /// \parblock
    /// In the examples radix sort is performed on a block of 128 threads, each thread provides
    /// two \p float value, results are returned using the same array as for input.
    ///
    /// \b HIP: \n
    /// \code{.cpp}
    /// __global__ void example_kernel(...)
    /// {
    ///     // specialize block_radix_sort for float, block of 128 threads,
    ///     // and two items per thread; key-only sort
    ///     using block_rsort_float = rocprim::block_radix_sort<float, 128, 2>;
    ///     // allocate storage in shared memory
    ///     __shared__ block_rsort_float::storage_type storage;
    ///
    ///     float input[2] = ...;
    ///     // execute block radix sort (descending)
    ///     block_rsort_float().sort_desc_to_striped(
    ///         input,
    ///         storage
    ///     );
    ///     ...
    /// }
    /// \endcode
    ///
    /// \b HC: \n
    /// \code{.cpp}
    /// hc::parallel_for_each(
    ///     hc::extent<1>(...).tile(128),
    ///     [=](hc::tiled_index<1> i) [[hc]]
    ///     {
    ///         // specialize block_radix_sort for float, block of 128 threads,
    ///         // and two items per thread; key-only sort
    ///         using block_rsort_float = rocprim::block_radix_sort<float, 128, 2>;
    ///
    ///         // allocate storage in shared memory
    ///         tile_static block_rsort_float::storage_type storage;
    ///
    ///         float input[2] = ...;
    ///         // execute block radix sort (descending)
    ///         block_rsort_float().sort_desc_to_striped(
    ///             input,
    ///             storage
    ///         );
    ///     }
    /// );
    /// \endcode
    /// If the \p input values across threads in a block are <tt>{[1, 2], [3, 4]  ..., [255, 256]}</tt>,
    /// then after sort they will be equal <tt>{[256, 128], ..., [130, 2], [129, 1]}</tt>.
    /// \endparblock
    ROCPRIM_DEVICE inline
    void sort_desc_to_striped(Key (&keys)[ItemsPerThread],
                              storage_type& storage,
                              unsigned int begin_bit = 0,
                              unsigned int end_bit = 8 * sizeof(Key))
    {
        empty_type * values = nullptr;
        sort_impl<true, true>(keys, values, storage, begin_bit, end_bit);
    }

    /// \overload
    /// \brief Performs descending radix sort over keys partitioned across threads in a block,
    /// results are saved in a striped arrangement.
    ///
    /// * This overload does not accept storage argument. Required shared memory is
    /// allocated by the method itself.
    ///
    /// \param [in, out] keys - reference to an array of keys provided by a thread.
    /// \param [in] begin_bit - [optional] index of the first (least significant) bit used in
    /// key comparison. Must be in range <tt>[0; 8 * sizeof(Key))</tt>. Default value: \p 0.
    /// \param [in] end_bit - [optional] past-the-end index (most significant) bit used in
    /// key comparison. Must be in range <tt>(begin_bit; 8 * sizeof(Key)]</tt>. Default
    /// value: \p <tt>8 * sizeof(Key)</tt>.
    ROCPRIM_DEVICE inline
    void sort_desc_to_striped(Key (&keys)[ItemsPerThread],
                              unsigned int begin_bit = 0,
                              unsigned int end_bit = 8 * sizeof(Key))
    {
        ROCPRIM_SHARED_MEMORY storage_type storage;
        sort_desc_to_striped(keys, storage, begin_bit, end_bit);
    }

    /// \brief Performs ascending radix sort over key-value pairs partitioned across
    /// threads in a block, results are saved in a striped arrangement.
    ///
    /// \pre Method is enabled only if \p Value type is different than empty_type.
    ///
    /// \param [in, out] keys - reference to an array of keys provided by a thread.
    /// \param [in, out] values - reference to an array of values provided by a thread.
    /// \param [in] storage - reference to a temporary storage object of type storage_type.
    /// \param [in] begin_bit - [optional] index of the first (least significant) bit used in
    /// key comparison. Must be in range <tt>[0; 8 * sizeof(Key))</tt>. Default value: \p 0.
    /// \param [in] end_bit - [optional] past-the-end index (most significant) bit used in
    /// key comparison. Must be in range <tt>(begin_bit; 8 * sizeof(Key)]</tt>. Default
    /// value: \p <tt>8 * sizeof(Key)</tt>.
    ///
    /// \par Storage reusage
    /// Synchronization barrier should be placed before \p storage is reused
    /// or repurposed: \p __syncthreads() in HIP, \p tile_barrier::wait() in HC, or
    /// universal rocprim::syncthreads().
    ///
    /// \par Examples
    /// \parblock
    /// In the examples radix sort is performed on a block of 4 threads, each thread provides
    /// two key-value <tt>int</tt>-<tt>float</tt> pairs, results are returned using the same
    /// arrays as for input.
    ///
    /// \b HIP: \n
    /// \code{.cpp}
    /// __global__ void example_kernel(...)
    /// {
    ///     // specialize block_radix_sort for int-float pairs, block of 4
    ///     // threads, and two items per thread
    ///     using block_rsort_ii = rocprim::block_radix_sort<int, 4, 2, int>;
    ///     // allocate storage in shared memory
    ///     __shared__ block_rsort_ii::storage_type storage;
    ///
    ///     int keys[2] = ...;
    ///     float values[2] = ...;
    ///     // execute block radix sort-by-key (ascending)
    ///     block_rsort_ii().sort_to_striped(
    ///         keys, values,
    ///         storage
    ///     );
    ///     ...
    /// }
    /// \endcode
    ///
    /// \b HC: \n
    /// \code{.cpp}
    /// hc::parallel_for_each(
    ///     hc::extent<1>(...).tile(4),
    ///     [=](hc::tiled_index<1> i) [[hc]]
    ///     {
    ///         // specialize block_radix_sort for int-float pairs, block of 4
    ///         // threads, and two items per thread
    ///         using block_rsort_ii = rocprim::block_radix_sort<int, 4, 2, int>;
    ///
    ///         // allocate storage in shared memory
    ///         tile_static block_rsort_ii::storage_type storage;
    ///
    ///         int keys[2] = ...;
    ///         float values[2] = ...;
    ///         // execute block radix sort-by-key (ascending)
    ///         block_rsort_ii().sort_to_striped(
    ///             keys, values,
    ///             storage
    ///         );
    ///         ...
    ///     }
    /// );
    /// \endcode
    /// If the \p keys across threads in a block are <tt>{[8, 7], [6, 5], [4, 3], [2, 1]}</tt> and
    /// the \p values are <tt>{[-1, -2], [-3, -4], [-5, -6], [-7, -8]}</tt>, then after sort the
    /// \p keys will be equal <tt>{[1, 5], [2, 6], [3, 7], [4, 8]}</tt> and the \p values will be
    /// equal <tt>{[-8, -4], [-7, -3], [-6, -2], [-5, -1]}</tt>.
    /// \endparblock
    template<bool WithValues = with_values>
    ROCPRIM_DEVICE inline
    void sort_to_striped(Key (&keys)[ItemsPerThread],
                         typename std::enable_if<WithValues, Value>::type (&values)[ItemsPerThread],
                         storage_type& storage,
                         unsigned int begin_bit = 0,
                         unsigned int end_bit = 8 * sizeof(Key))
    {
        sort_impl<false, true>(keys, values, storage, begin_bit, end_bit);
    }

    /// \overload
    /// \brief Performs ascending radix sort over key-value pairs partitioned across
    /// threads in a block, results are saved in a striped arrangement.
    ///
    /// * This overload does not accept storage argument. Required shared memory is
    /// allocated by the method itself.
    ///
    /// \param [in, out] keys - reference to an array of keys provided by a thread.
    /// \param [in, out] values - reference to an array of values provided by a thread.
    /// \param [in] begin_bit - [optional] index of the first (least significant) bit used in
    /// key comparison. Must be in range <tt>[0; 8 * sizeof(Key))</tt>. Default value: \p 0.
    /// \param [in] end_bit - [optional] past-the-end index (most significant) bit used in
    /// key comparison. Must be in range <tt>(begin_bit; 8 * sizeof(Key)]</tt>. Default
    /// value: \p <tt>8 * sizeof(Key)</tt>.
    template<bool WithValues = with_values>
    ROCPRIM_DEVICE inline
    void sort_to_striped(Key (&keys)[ItemsPerThread],
                         typename std::enable_if<WithValues, Value>::type (&values)[ItemsPerThread],
                         unsigned int begin_bit = 0,
                         unsigned int end_bit = 8 * sizeof(Key))
    {
        ROCPRIM_SHARED_MEMORY storage_type storage;
        sort_to_striped(keys, values, storage, begin_bit, end_bit);
    }

    /// \brief Performs descending radix sort over key-value pairs partitioned across
    /// threads in a block, results are saved in a striped arrangement.
    ///
    /// \pre Method is enabled only if \p Value type is different than empty_type.
    ///
    /// \param [in, out] keys - reference to an array of keys provided by a thread.
    /// \param [in, out] values - reference to an array of values provided by a thread.
    /// \param [in] storage - reference to a temporary storage object of type storage_type.
    /// \param [in] begin_bit - [optional] index of the first (least significant) bit used in
    /// key comparison. Must be in range <tt>[0; 8 * sizeof(Key))</tt>. Default value: \p 0.
    /// \param [in] end_bit - [optional] past-the-end index (most significant) bit used in
    /// key comparison. Must be in range <tt>(begin_bit; 8 * sizeof(Key)]</tt>. Default
    /// value: \p <tt>8 * sizeof(Key)</tt>.
    ///
    /// \par Storage reusage
    /// Synchronization barrier should be placed before \p storage is reused
    /// or repurposed: \p __syncthreads() in HIP, \p tile_barrier::wait() in HC, or
    /// universal rocprim::syncthreads().
    ///
    /// \par Examples
    /// \parblock
    /// In the examples radix sort is performed on a block of 4 threads, each thread provides
    /// two key-value <tt>int</tt>-<tt>float</tt> pairs, results are returned using the same
    /// arrays as for input.
    ///
    /// \b HIP: \n
    /// \code{.cpp}
    /// __global__ void example_kernel(...)
    /// {
    ///     // specialize block_radix_sort for int-float pairs, block of 4
    ///     // threads, and two items per thread
    ///     using block_rsort_ii = rocprim::block_radix_sort<int, 4, 2, int>;
    ///     // allocate storage in shared memory
    ///     __shared__ block_rsort_ii::storage_type storage;
    ///
    ///     int keys[2] = ...;
    ///     float values[2] = ...;
    ///     // execute block radix sort-by-key (descending)
    ///     block_rsort_ii().sort_desc_to_striped(
    ///         keys, values,
    ///         storage
    ///     );
    ///     ...
    /// }
    /// \endcode
    ///
    /// \b HC: \n
    /// \code{.cpp}
    /// hc::parallel_for_each(
    ///     hc::extent<1>(...).tile(4),
    ///     [=](hc::tiled_index<1> i) [[hc]]
    ///     {
    ///         // specialize block_radix_sort for int-float pairs, block of 4
    ///         // threads, and two items per thread
    ///         using block_rsort_ii = rocprim::block_radix_sort<int, 4, 2, int>;
    ///
    ///         // allocate storage in shared memory
    ///         tile_static block_rsort_ii::storage_type storage;
    ///
    ///         int keys[2] = ...;
    ///         float values[2] = ...;
    ///         // execute block radix sort-by-key (descending)
    ///         block_rsort_ii().sort_desc_to_striped(
    ///             keys, values,
    ///             storage
    ///         );
    ///         ...
    ///     }
    /// );
    /// \endcode
    /// If the \p keys across threads in a block are <tt>{[1, 2], [3, 4], [5, 6], [7, 8]}</tt> and
    /// the \p values are <tt>{[80, 70], [60, 50], [40, 30], [20, 10]}</tt>, then after sort the
    /// \p keys will be equal <tt>{[8, 4], [7, 3], [6, 2], [5, 1]}</tt> and the \p values will be
    /// equal <tt>{[10, 50], [20, 60], [30, 70], [40, 80]}</tt>.
    /// \endparblock
    template<bool WithValues = with_values>
    ROCPRIM_DEVICE inline
    void sort_desc_to_striped(Key (&keys)[ItemsPerThread],
                              typename std::enable_if<WithValues, Value>::type (&values)[ItemsPerThread],
                              storage_type& storage,
                              unsigned int begin_bit = 0,
                              unsigned int end_bit = 8 * sizeof(Key))
    {
        sort_impl<true, true>(keys, values, storage, begin_bit, end_bit);
    }

    /// \overload
    /// \brief Performs descending radix sort over key-value pairs partitioned across
    /// threads in a block, results are saved in a striped arrangement.
    ///
    /// * This overload does not accept storage argument. Required shared memory is
    /// allocated by the method itself.
    ///
    /// \param [in, out] keys - reference to an array of keys provided by a thread.
    /// \param [in, out] values - reference to an array of values provided by a thread.
    /// \param [in] begin_bit - [optional] index of the first (least significant) bit used in
    /// key comparison. Must be in range <tt>[0; 8 * sizeof(Key))</tt>. Default value: \p 0.
    /// \param [in] end_bit - [optional] past-the-end index (most significant) bit used in
    /// key comparison. Must be in range <tt>(begin_bit; 8 * sizeof(Key)]</tt>. Default
    /// value: \p <tt>8 * sizeof(Key)</tt>.
    template<bool WithValues = with_values>
    ROCPRIM_DEVICE inline
    void sort_desc_to_striped(Key (&keys)[ItemsPerThread],
                              typename std::enable_if<WithValues, Value>::type (&values)[ItemsPerThread],
                              unsigned int begin_bit = 0,
                              unsigned int end_bit = 8 * sizeof(Key))
    {
        ROCPRIM_SHARED_MEMORY storage_type storage;
        sort_desc_to_striped(keys, values, storage, begin_bit, end_bit);
    }

private:

    template<bool Descending, bool ToStriped = false, class SortedValue>
    ROCPRIM_DEVICE inline
    void sort_impl(Key (&keys)[ItemsPerThread],
                   SortedValue * values,
                   storage_type& storage,
                   unsigned int begin_bit,
                   unsigned int end_bit)
    {
        using key_codec = ::rocprim::detail::radix_key_codec<Key, Descending>;

        const unsigned int flat_id = ::rocprim::flat_block_thread_id();

        bit_key_type bit_keys[ItemsPerThread];
        for(unsigned int i = 0; i < ItemsPerThread; i++)
        {
            bit_keys[i] = key_codec::encode(keys[i]);
        }

        for(unsigned int bit = begin_bit; bit < end_bit; bit += RadixBits)
        {
            buckets banks[ItemsPerThread];
            buckets positions[ItemsPerThread];
            buckets counts;

            // Handle cases when (end_bit - bit) is not divisible by RadixBits, i.e. the last
            // iteration has a shorter mask.
            const unsigned int radix_mask = (1u << ::rocprim::min(RadixBits, end_bit - bit)) - 1;

            for(unsigned int i = 0; i < ItemsPerThread; i++)
            {
                const unsigned int radix = (bit_keys[i] >> bit) & radix_mask;
                for(unsigned int r = 0; r < radix_size - 1; r++)
                {
                    banks[i][r] = radix == r;
                }
            }
            bit_block_scan().exclusive_scan(banks, positions, counts, storage.bit_block_scan);

            // Prefix sum of counts to compute starting positions of keys of each radix value
            buckets starts;
            unsigned int last_start = 0;
            for(unsigned int r = 0; r < radix_size - 1; r++)
            {
                const unsigned int c = counts[r];
                starts[r] = last_start;
                last_start += c;
            }
            // Scatter keys to computed positions considering starting positions of their
            // radix values
            unsigned int ranks[ItemsPerThread];
            for(unsigned int i = 0; i < ItemsPerThread; i++)
            {
                const unsigned int radix = (bit_keys[i] >> bit) & radix_mask;
                unsigned int to_position = 0;
                unsigned int last_position = 0;
                for(unsigned int r = 0; r < radix_size - 1; r++)
                {
                    to_position = radix == r ? (starts[r] + positions[i][r]) : to_position;
                    last_position += positions[i][r];
                }
                // Calculate position for the last radix value based on positions of
                // all other previous values
                const unsigned int from_position = flat_id * ItemsPerThread + i;
                ranks[i] = radix == radix_size - 1
                    ? (last_start + from_position - last_position)
                    : to_position;
            }
            exchange_keys(storage, bit_keys, ranks);
            exchange_values(storage, values, ranks);
        }

        if(ToStriped)
        {
            to_striped_keys(storage, bit_keys);
            to_striped_values(storage, values);
        }

        for(unsigned int i = 0; i < ItemsPerThread; i++)
        {
            keys[i] = key_codec::decode(bit_keys[i]);
        }
    }

    ROCPRIM_DEVICE inline
    void exchange_keys(storage_type& storage,
                       bit_key_type (&bit_keys)[ItemsPerThread],
                       const unsigned int (&ranks)[ItemsPerThread])
    {
        // Synchronization is omitted here because bit_block_scan already calls it
        bit_keys_exchange_type().scatter_to_blocked(bit_keys, bit_keys, ranks, storage.bit_keys_exchange);
    }

    template<class SortedValue>
    ROCPRIM_DEVICE inline
    void exchange_values(storage_type& storage,
                         SortedValue * values,
                         const unsigned int (&ranks)[ItemsPerThread])
    {
        ::rocprim::syncthreads(); // Storage will be reused (union), synchronization is needed
        SortedValue (&vs)[ItemsPerThread] = *reinterpret_cast<SortedValue (*)[ItemsPerThread]>(values);
        values_exchange_type().scatter_to_blocked(vs, vs, ranks, storage.values_exchange);
    }

    ROCPRIM_DEVICE inline
    void exchange_values(storage_type& storage,
                         empty_type * values,
                         const unsigned int (&ranks)[ItemsPerThread])
    {
        (void) storage;
        (void) values;
        (void) ranks;
    }

    ROCPRIM_DEVICE inline
    void to_striped_keys(storage_type& storage,
                         bit_key_type (&bit_keys)[ItemsPerThread])
    {
        ::rocprim::syncthreads();
        bit_keys_exchange_type().blocked_to_striped(bit_keys, bit_keys, storage.bit_keys_exchange);
    }

    template<class SortedValue>
    ROCPRIM_DEVICE inline
    void to_striped_values(storage_type& storage,
                           SortedValue * values)
    {
        ::rocprim::syncthreads(); // Storage will be reused (union), synchronization is needed
        SortedValue (&vs)[ItemsPerThread] = *reinterpret_cast<SortedValue (*)[ItemsPerThread]>(values);
        values_exchange_type().blocked_to_striped(vs, vs, storage.values_exchange);
    }

    ROCPRIM_DEVICE inline
    void to_striped_values(storage_type& storage,
                           empty_type * values)
    {
        (void) storage;
        (void) values;
    }
};

END_ROCPRIM_NAMESPACE

/// @}
// end of group collectiveblockmodule

#endif // ROCPRIM_BLOCK_BLOCK_RADIX_SORT_HPP_
