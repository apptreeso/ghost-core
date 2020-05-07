// Copyright (c) 2019-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
#include <sync.h>
#include <test/util/setup_common.h>
#include <txmempool.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(validation_flush_tests, BasicTestingSetup)

//! Test utilities for detecting when we need to flush the coins cache based
//! on estimated memory usage.
//!
//! @sa CChainState::GetCoinsCacheSizeState()
//!
BOOST_AUTO_TEST_CASE(getcoinscachesizestate)
{
    BlockManager blockman{};
    CChainState chainstate{blockman};
    chainstate.InitCoinsDB(/*cache_size_bytes*/ 1 << 10, /*in_memory*/ true, /*should_wipe*/ false);
    WITH_LOCK(::cs_main, chainstate.InitCoinsCache());
    CTxMemPool tx_pool{};

    constexpr bool is_64_bit = sizeof(void*) == 8;

    LOCK(::cs_main);
    auto& view = chainstate.CoinsTip();

    //! Create and add a Coin with DynamicMemoryUsage of 80 bytes to the given view.
    auto add_coin = [](CCoinsViewCache& coins_view) -> COutPoint {
        Coin newcoin;
        uint256 txid = InsecureRand256();
        COutPoint outp{txid, 0};
        newcoin.nHeight = 1;
        newcoin.out.nValue = InsecureRand32();
        newcoin.out.scriptPubKey.assign((uint32_t)56, 1);
        coins_view.AddCoin(outp, std::move(newcoin), false);

        return outp;
    };

    // The number of bytes consumed by coin's heap data, i.e. CScript
    // (prevector<28, unsigned char>) when assigned 56 bytes of data per above.
    //
    // See also: Coin::DynamicMemoryUsage().
    constexpr int COIN_SIZE = is_64_bit ? 80 : 64;

    auto print_view_mem_usage = [](CCoinsViewCache& view) {
        BOOST_TEST_MESSAGE("CCoinsViewCache memory usage: " << view.DynamicMemoryUsage());
    };

    constexpr size_t MAX_COINS_CACHE_BYTES = 1024;

    // Without any coins in the cache, we shouldn't need to flush.
    BOOST_CHECK_EQUAL(
        chainstate.GetCoinsCacheSizeState(tx_pool, MAX_COINS_CACHE_BYTES, /*max_mempool_size_bytes*/ 0),
        CoinsCacheSizeState::OK);

    // If the initial memory allocations of cacheCoins don't match these common
    // cases, we can't really continue to make assertions about memory usage.
    // End the test early.
    if (view.DynamicMemoryUsage() != 32 && view.DynamicMemoryUsage() != 16) {
        // Add a bunch of coins to see that we at least flip over to CRITICAL.

        for (int i{0}; i < 1000; ++i) {
            COutPoint res = add_coin(view);
            BOOST_CHECK_EQUAL(view.AccessCoin(res).DynamicMemoryUsage(), COIN_SIZE);
        }

        BOOST_CHECK_EQUAL(
            chainstate.GetCoinsCacheSizeState(tx_pool, MAX_COINS_CACHE_BYTES, /*max_mempool_size_bytes*/ 0),
            CoinsCacheSizeState::CRITICAL);

        BOOST_TEST_MESSAGE("Exiting cache flush tests early due to unsupported arch");
        return;
    }

    print_view_mem_usage(view);
    BOOST_CHECK_EQUAL(view.DynamicMemoryUsage(), is_64_bit ? 32 : 16);

    // We should be able to add COINS_UNTIL_CRITICAL coins to the cache before going CRITICAL.
    // This is contingent not only on the dynamic memory usage of the Coins
    // that we're adding (COIN_SIZE bytes per), but also on how much memory the
    // cacheCoins (unordered_map) preallocates.
    //
    // I came up with the count by examining the printed memory usage of the
    // CCoinsCacheView, so it's sort of arbitrary - but it shouldn't change
    // unless we somehow change the way the cacheCoins map allocates memory.
    //
    constexpr int COINS_UNTIL_CRITICAL = is_64_bit ? 3 : 3;

    for (int i{0}; i < COINS_UNTIL_CRITICAL; ++i) {
        COutPoint res = add_coin(view);
        print_view_mem_usage(view);
        BOOST_CHECK_EQUAL(view.AccessCoin(res).DynamicMemoryUsage(), COIN_SIZE);
        BOOST_CHECK_EQUAL(
            chainstate.GetCoinsCacheSizeState(tx_pool, MAX_COINS_CACHE_BYTES, /*max_mempool_size_bytes*/ 0),
            CoinsCacheSizeState::OK);
    }

    // Adding some additional coins will push us over the edge to CRITICAL.
    for (int i{0}; i < 4; ++i) {
        add_coin(view);
        print_view_mem_usage(view);
        if (chainstate.GetCoinsCacheSizeState(tx_pool, MAX_COINS_CACHE_BYTES, /*max_mempool_size_bytes*/ 0) ==
            CoinsCacheSizeState::CRITICAL) {
            break;
        }
    }

    BOOST_CHECK_EQUAL(
        chainstate.GetCoinsCacheSizeState(tx_pool, MAX_COINS_CACHE_BYTES, /*max_mempool_size_bytes*/ 0),
        CoinsCacheSizeState::CRITICAL);

    // COutpoint can take up to 288 bytes, to test the CoinsCacheSizeState::LARGE
    // reliably, the interval between 90% and 100% must fit atleast one output.
    // 0.1 * min_size >= 288 bytes results in a minimum size of 2880
    constexpr size_t EXPAND_COINS_CACHE_BY_MEMPOOL = 2880 - MAX_COINS_CACHE_BYTES;

    // Passing non-zero max mempool usage should allow us more headroom.
    BOOST_CHECK_EQUAL(
        chainstate.GetCoinsCacheSizeState(tx_pool, MAX_COINS_CACHE_BYTES, /*max_mempool_size_bytes*/ EXPAND_COINS_CACHE_BY_MEMPOOL),
        CoinsCacheSizeState::OK);

    for (int i{0}; i < 2; ++i) {
        add_coin(view);
        print_view_mem_usage(view);
        BOOST_CHECK_EQUAL(
            chainstate.GetCoinsCacheSizeState(tx_pool, MAX_COINS_CACHE_BYTES, /*max_mempool_size_bytes*/ EXPAND_COINS_CACHE_BY_MEMPOOL),
            CoinsCacheSizeState::OK);
    }

    // Adding some additional coins will push us over the edge to LARGE.
    for (int i{0}; i < 10; ++i) {
        add_coin(view);
        print_view_mem_usage(view);
        if (chainstate.GetCoinsCacheSizeState(tx_pool, MAX_COINS_CACHE_BYTES, /*max_mempool_size_bytes*/ EXPAND_COINS_CACHE_BY_MEMPOOL) ==
            CoinsCacheSizeState::LARGE) {
            break;
        }
    }

    float usage_percentage = (float)view.DynamicMemoryUsage() / (MAX_COINS_CACHE_BYTES + EXPAND_COINS_CACHE_BY_MEMPOOL);
    BOOST_TEST_MESSAGE("CoinsTip usage percentage: " << usage_percentage);
    BOOST_CHECK(usage_percentage >= 0.9);
    BOOST_CHECK(usage_percentage < 1);
    BOOST_CHECK_EQUAL(
        chainstate.GetCoinsCacheSizeState(tx_pool, MAX_COINS_CACHE_BYTES, EXPAND_COINS_CACHE_BY_MEMPOOL),
        CoinsCacheSizeState::LARGE);

    // Using the default max_* values permits way more coins to be added.
    for (int i{0}; i < 1000; ++i) {
        add_coin(view);
        BOOST_CHECK_EQUAL(
            chainstate.GetCoinsCacheSizeState(tx_pool),
            CoinsCacheSizeState::OK);
    }

    // Flushing the view doesn't take us back to OK because cacheCoins has
    // preallocated memory that doesn't get reclaimed even after flush.

    BOOST_CHECK_EQUAL(
        chainstate.GetCoinsCacheSizeState(tx_pool, MAX_COINS_CACHE_BYTES, 0),
        CoinsCacheSizeState::CRITICAL);

    view.SetBestBlock(InsecureRand256(), 5);
    BOOST_CHECK(view.Flush());
    print_view_mem_usage(view);

    BOOST_CHECK_EQUAL(
        chainstate.GetCoinsCacheSizeState(tx_pool, MAX_COINS_CACHE_BYTES, 0),
        CoinsCacheSizeState::CRITICAL);
}

BOOST_AUTO_TEST_SUITE_END()
