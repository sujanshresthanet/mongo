/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/client/read_preference.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/logical_session_cache_noop.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/s/config/config_server_test_fixture.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/s/transaction_coordinator_service.h"
#include "mongo/db/session_catalog_mongod.h"
#include "mongo/s/catalog/type_chunk.h"

namespace mongo {
namespace {
using unittest::assertGet;

class SplitChunkTest : public ConfigServerTestFixture {
protected:
    std::string _shardName = "shard0000";
    void setUp() override {
        ConfigServerTestFixture::setUp();
        ShardType shard;
        shard.setName(_shardName);
        shard.setHost(_shardName + ":12");
        setupShards({shard});

        DBDirectClient client(operationContext());
        client.createCollection(NamespaceString::kSessionTransactionsTableNamespace.ns());
        client.createIndexes(NamespaceString::kSessionTransactionsTableNamespace.ns(),
                             {MongoDSessionCatalog::getConfigTxnPartialIndexSpec()});

        LogicalSessionCache::set(getServiceContext(), std::make_unique<LogicalSessionCacheNoop>());
        TransactionCoordinatorService::get(operationContext())
            ->onShardingInitialization(operationContext(), true);
    }

    void tearDown() override {
        TransactionCoordinatorService::get(operationContext())->onStepDown();
        ConfigServerTestFixture::tearDown();
    }

    const NamespaceString _nss1{"TestDB", "TestColl1"};
    const NamespaceString _nss2{"TestDB", "TestColl2"};
    const KeyPattern _keyPattern{BSON("a" << 1)};
};

TEST_F(SplitChunkTest, SplitExistingChunkCorrectlyShouldSucceed) {
    auto test = [&](const NamespaceString& nss, const Timestamp& collTimestamp) {
        const auto collEpoch = OID::gen();
        const auto collUuid = UUID::gen();

        ChunkType chunk;
        chunk.setName(OID::gen());
        chunk.setCollectionUUID(collUuid);

        auto origVersion = ChunkVersion(1, 0, collEpoch, collTimestamp);
        chunk.setVersion(origVersion);
        chunk.setShard(ShardId(_shardName));

        auto chunkMin = BSON("a" << 1);
        auto chunkMax = BSON("a" << 10);
        chunk.setMin(chunkMin);
        chunk.setMax(chunkMax);
        chunk.setHistory({ChunkHistory(Timestamp(100, 0), ShardId(_shardName)),
                          ChunkHistory(Timestamp(90, 0), ShardId("shardY"))});

        auto chunkSplitPoint = BSON("a" << 5);
        std::vector<BSONObj> splitPoints{chunkSplitPoint};

        setupCollection(nss, _keyPattern, {chunk});

        auto versions = assertGet(ShardingCatalogManager::get(operationContext())
                                      ->commitChunkSplit(operationContext(),
                                                         nss,
                                                         collEpoch,
                                                         collTimestamp,
                                                         ChunkRange(chunkMin, chunkMax),
                                                         splitPoints,
                                                         "shard0000",
                                                         false /* fromChunkSplitter*/));
        auto collVersion = ChunkVersion::parse(versions["collectionVersion"]);
        auto shardVersion = ChunkVersion::parse(versions["shardVersion"]);

        ASSERT_TRUE(origVersion.isOlderThan(shardVersion));
        ASSERT_EQ(collVersion, shardVersion);

        // Check for increment on mergedChunk's minor version
        auto expectedShardVersion = ChunkVersion(
            origVersion.majorVersion(), origVersion.minorVersion() + 2, collEpoch, collTimestamp);
        ASSERT_EQ(expectedShardVersion, shardVersion);
        ASSERT_EQ(shardVersion, collVersion);

        // First chunkDoc should have range [chunkMin, chunkSplitPoint]
        auto chunkDocStatus =
            getChunkDoc(operationContext(), collUuid, chunkMin, collEpoch, collTimestamp);
        ASSERT_OK(chunkDocStatus.getStatus());

        auto chunkDoc = chunkDocStatus.getValue();
        ASSERT_BSONOBJ_EQ(chunkSplitPoint, chunkDoc.getMax());

        // Check for increment on first chunkDoc's minor version
        ASSERT_EQ(origVersion.majorVersion(), chunkDoc.getVersion().majorVersion());
        ASSERT_EQ(origVersion.minorVersion() + 1, chunkDoc.getVersion().minorVersion());

        // Make sure the history is there
        ASSERT_EQ(2UL, chunkDoc.getHistory().size());

        // Second chunkDoc should have range [chunkSplitPoint, chunkMax]
        auto otherChunkDocStatus =
            getChunkDoc(operationContext(), collUuid, chunkSplitPoint, collEpoch, collTimestamp);
        ASSERT_OK(otherChunkDocStatus.getStatus());

        auto otherChunkDoc = otherChunkDocStatus.getValue();
        ASSERT_BSONOBJ_EQ(chunkMax, otherChunkDoc.getMax());

        // Check for increment on second chunkDoc's minor version
        ASSERT_EQ(origVersion.majorVersion(), otherChunkDoc.getVersion().majorVersion());
        ASSERT_EQ(origVersion.minorVersion() + 2, otherChunkDoc.getVersion().minorVersion());

        // Make sure the history is there
        ASSERT_EQ(2UL, otherChunkDoc.getHistory().size());

        // Both chunks should have the same history
        ASSERT(chunkDoc.getHistory() == otherChunkDoc.getHistory());
    };

    test(_nss2, Timestamp(42));
}

TEST_F(SplitChunkTest, MultipleSplitsOnExistingChunkShouldSucceed) {
    auto test = [&](const NamespaceString& nss, const Timestamp& collTimestamp) {
        const auto collEpoch = OID::gen();
        const auto collUuid = UUID::gen();

        ChunkType chunk;
        chunk.setName(OID::gen());
        chunk.setCollectionUUID(collUuid);

        auto origVersion = ChunkVersion(1, 0, collEpoch, collTimestamp);
        chunk.setVersion(origVersion);
        chunk.setShard(ShardId(_shardName));

        auto chunkMin = BSON("a" << 1);
        auto chunkMax = BSON("a" << 10);
        chunk.setMin(chunkMin);
        chunk.setMax(chunkMax);
        chunk.setHistory({ChunkHistory(Timestamp(100, 0), ShardId(_shardName)),
                          ChunkHistory(Timestamp(90, 0), ShardId("shardY"))});

        auto chunkSplitPoint = BSON("a" << 5);
        auto chunkSplitPoint2 = BSON("a" << 7);
        std::vector<BSONObj> splitPoints{chunkSplitPoint, chunkSplitPoint2};

        setupCollection(nss, _keyPattern, {chunk});

        ASSERT_OK(ShardingCatalogManager::get(operationContext())
                      ->commitChunkSplit(operationContext(),
                                         nss,
                                         collEpoch,
                                         collTimestamp,
                                         ChunkRange(chunkMin, chunkMax),
                                         splitPoints,
                                         "shard0000",
                                         false /* fromChunkSplitter*/));

        // First chunkDoc should have range [chunkMin, chunkSplitPoint]
        auto chunkDocStatus =
            getChunkDoc(operationContext(), collUuid, chunkMin, collEpoch, collTimestamp);
        ASSERT_OK(chunkDocStatus.getStatus());

        auto chunkDoc = chunkDocStatus.getValue();
        ASSERT_BSONOBJ_EQ(chunkSplitPoint, chunkDoc.getMax());

        // Check for increment on first chunkDoc's minor version
        ASSERT_EQ(origVersion.majorVersion(), chunkDoc.getVersion().majorVersion());
        ASSERT_EQ(origVersion.minorVersion() + 1, chunkDoc.getVersion().minorVersion());

        // Make sure the history is there
        ASSERT_EQ(2UL, chunkDoc.getHistory().size());

        // Second chunkDoc should have range [chunkSplitPoint, chunkSplitPoint2]
        auto midChunkDocStatus =
            getChunkDoc(operationContext(), collUuid, chunkSplitPoint, collEpoch, collTimestamp);
        ASSERT_OK(midChunkDocStatus.getStatus());

        auto midChunkDoc = midChunkDocStatus.getValue();
        ASSERT_BSONOBJ_EQ(chunkSplitPoint2, midChunkDoc.getMax());

        // Check for increment on second chunkDoc's minor version
        ASSERT_EQ(origVersion.majorVersion(), midChunkDoc.getVersion().majorVersion());
        ASSERT_EQ(origVersion.minorVersion() + 2, midChunkDoc.getVersion().minorVersion());

        // Make sure the history is there
        ASSERT_EQ(2UL, midChunkDoc.getHistory().size());

        // Third chunkDoc should have range [chunkSplitPoint2, chunkMax]
        auto lastChunkDocStatus =
            getChunkDoc(operationContext(), collUuid, chunkSplitPoint2, collEpoch, collTimestamp);
        ASSERT_OK(lastChunkDocStatus.getStatus());

        auto lastChunkDoc = lastChunkDocStatus.getValue();
        ASSERT_BSONOBJ_EQ(chunkMax, lastChunkDoc.getMax());

        // Check for increment on third chunkDoc's minor version
        ASSERT_EQ(origVersion.majorVersion(), lastChunkDoc.getVersion().majorVersion());
        ASSERT_EQ(origVersion.minorVersion() + 3, lastChunkDoc.getVersion().minorVersion());

        // Make sure the history is there
        ASSERT_EQ(2UL, lastChunkDoc.getHistory().size());

        // Both chunks should have the same history
        ASSERT(chunkDoc.getHistory() == midChunkDoc.getHistory());
        ASSERT(midChunkDoc.getHistory() == lastChunkDoc.getHistory());
    };

    test(_nss2, Timestamp(42));
}

TEST_F(SplitChunkTest, NewSplitShouldClaimHighestVersion) {
    auto test = [&](const NamespaceString& nss, const Timestamp& collTimestamp) {
        const auto collEpoch = OID::gen();
        const auto collUuid = UUID::gen();

        ChunkType chunk, chunk2;
        chunk.setName(OID::gen());
        chunk.setCollectionUUID(collUuid);
        chunk2.setName(OID::gen());
        chunk2.setCollectionUUID(collUuid);

        // set up first chunk
        auto origVersion = ChunkVersion(1, 2, collEpoch, collTimestamp);
        chunk.setVersion(origVersion);
        chunk.setShard(ShardId(_shardName));

        auto chunkMin = BSON("a" << 1);
        auto chunkMax = BSON("a" << 10);
        chunk.setMin(chunkMin);
        chunk.setMax(chunkMax);

        std::vector<BSONObj> splitPoints;
        auto chunkSplitPoint = BSON("a" << 5);
        splitPoints.push_back(chunkSplitPoint);

        // set up second chunk (chunk2)
        auto competingVersion = ChunkVersion(2, 1, collEpoch, collTimestamp);
        chunk2.setVersion(competingVersion);
        chunk2.setShard(ShardId(_shardName));
        chunk2.setMin(BSON("a" << 10));
        chunk2.setMax(BSON("a" << 20));

        setupCollection(nss, _keyPattern, {chunk, chunk2});

        ASSERT_OK(ShardingCatalogManager::get(operationContext())
                      ->commitChunkSplit(operationContext(),
                                         nss,
                                         collEpoch,
                                         collTimestamp,
                                         ChunkRange(chunkMin, chunkMax),
                                         splitPoints,
                                         "shard0000",
                                         false /* fromChunkSplitter*/));

        // First chunkDoc should have range [chunkMin, chunkSplitPoint]
        auto chunkDocStatus =
            getChunkDoc(operationContext(), collUuid, chunkMin, collEpoch, collTimestamp);
        ASSERT_OK(chunkDocStatus.getStatus());

        auto chunkDoc = chunkDocStatus.getValue();
        ASSERT_BSONOBJ_EQ(chunkSplitPoint, chunkDoc.getMax());

        // Check for increment based on the competing chunk version
        ASSERT_EQ(competingVersion.majorVersion(), chunkDoc.getVersion().majorVersion());
        ASSERT_EQ(competingVersion.minorVersion() + 1, chunkDoc.getVersion().minorVersion());

        // Second chunkDoc should have range [chunkSplitPoint, chunkMax]
        auto otherChunkDocStatus =
            getChunkDoc(operationContext(), collUuid, chunkSplitPoint, collEpoch, collTimestamp);
        ASSERT_OK(otherChunkDocStatus.getStatus());

        auto otherChunkDoc = otherChunkDocStatus.getValue();
        ASSERT_BSONOBJ_EQ(chunkMax, otherChunkDoc.getMax());

        // Check for increment based on the competing chunk version
        ASSERT_EQ(competingVersion.majorVersion(), otherChunkDoc.getVersion().majorVersion());
        ASSERT_EQ(competingVersion.minorVersion() + 2, otherChunkDoc.getVersion().minorVersion());
    };

    test(_nss2, Timestamp(42));
}

TEST_F(SplitChunkTest, PreConditionFailErrors) {
    auto test = [&](const NamespaceString& nss, const Timestamp& collTimestamp) {
        const auto collEpoch = OID::gen();

        ChunkType chunk;
        chunk.setName(OID::gen());
        chunk.setCollectionUUID(UUID::gen());

        auto origVersion = ChunkVersion(1, 0, collEpoch, collTimestamp);
        chunk.setVersion(origVersion);
        chunk.setShard(ShardId(_shardName));

        auto chunkMin = BSON("a" << 1);
        auto chunkMax = BSON("a" << 10);
        chunk.setMin(chunkMin);
        chunk.setMax(chunkMax);

        std::vector<BSONObj> splitPoints;
        auto chunkSplitPoint = BSON("a" << 5);
        splitPoints.push_back(chunkSplitPoint);

        setupCollection(nss, _keyPattern, {chunk});

        ASSERT_THROWS_CODE(ShardingCatalogManager::get(operationContext())
                               ->commitChunkSplit(operationContext(),
                                                  nss,
                                                  collEpoch,
                                                  collTimestamp,
                                                  ChunkRange(chunkMin, BSON("a" << 7)),
                                                  splitPoints,
                                                  "shard0000",
                                                  false /* fromChunkSplitter*/),
                           DBException,
                           ErrorCodes::BadValue);
    };

    test(_nss2, Timestamp(42));
}

TEST_F(SplitChunkTest, NonExisingNamespaceErrors) {
    auto test = [&](const NamespaceString& nss, const Timestamp& collTimestamp) {
        const auto collEpoch = OID::gen();

        ChunkType chunk;
        chunk.setCollectionUUID(UUID::gen());

        auto origVersion = ChunkVersion(1, 0, collEpoch, collTimestamp);
        chunk.setVersion(origVersion);
        chunk.setShard(ShardId(_shardName));

        auto chunkMin = BSON("a" << 1);
        auto chunkMax = BSON("a" << 10);
        chunk.setMin(chunkMin);
        chunk.setMax(chunkMax);

        std::vector<BSONObj> splitPoints{BSON("a" << 5)};

        setupCollection(nss, _keyPattern, {chunk});

        ASSERT_THROWS_WHAT(ShardingCatalogManager::get(operationContext())
                               ->commitChunkSplit(operationContext(),
                                                  NamespaceString("TestDB.NonExistingColl"),
                                                  collEpoch,
                                                  Timestamp{50, 0},
                                                  ChunkRange(chunkMin, chunkMax),
                                                  splitPoints,
                                                  "shard0000",
                                                  false /* fromChunkSplitter*/),
                           DBException,
                           "Collection does not exist");
    };

    test(_nss2, Timestamp(42));
}

TEST_F(SplitChunkTest, NonMatchingEpochsOfChunkAndRequestErrors) {
    auto test = [&](const NamespaceString& nss, const Timestamp& collTimestamp) {
        const auto collEpoch = OID::gen();

        ChunkType chunk;
        chunk.setCollectionUUID(UUID::gen());

        auto origVersion = ChunkVersion(1, 0, collEpoch, collTimestamp);
        chunk.setVersion(origVersion);
        chunk.setShard(ShardId(_shardName));

        auto chunkMin = BSON("a" << 1);
        auto chunkMax = BSON("a" << 10);
        chunk.setMin(chunkMin);
        chunk.setMax(chunkMax);

        std::vector<BSONObj> splitPoints{BSON("a" << 5)};

        setupCollection(nss, _keyPattern, {chunk});

        auto splitStatus = ShardingCatalogManager::get(operationContext())
                               ->commitChunkSplit(operationContext(),
                                                  nss,
                                                  OID::gen(),
                                                  Timestamp{50, 0},
                                                  ChunkRange(chunkMin, chunkMax),
                                                  splitPoints,
                                                  "shard0000",
                                                  false /* fromChunkSplitter*/);
        ASSERT_EQ(ErrorCodes::StaleEpoch, splitStatus);
    };

    test(_nss2, Timestamp(42));
}

TEST_F(SplitChunkTest, SplitPointsOutOfOrderShouldFail) {
    auto test = [&](const NamespaceString& nss, const Timestamp& collTimestamp) {
        const auto collEpoch = OID::gen();

        ChunkType chunk;
        chunk.setName(OID::gen());
        chunk.setCollectionUUID(UUID::gen());

        auto origVersion = ChunkVersion(1, 0, collEpoch, collTimestamp);
        chunk.setVersion(origVersion);
        chunk.setShard(ShardId(_shardName));

        auto chunkMin = BSON("a" << 1);
        auto chunkMax = BSON("a" << 10);
        chunk.setMin(chunkMin);
        chunk.setMax(chunkMax);

        std::vector<BSONObj> splitPoints{BSON("a" << 5), BSON("a" << 4)};

        setupCollection(nss, _keyPattern, {chunk});

        ASSERT_THROWS_CODE(ShardingCatalogManager::get(operationContext())
                               ->commitChunkSplit(operationContext(),
                                                  nss,
                                                  collEpoch,
                                                  collTimestamp,
                                                  ChunkRange(chunkMin, chunkMax),
                                                  splitPoints,
                                                  "shard0000",
                                                  false /* fromChunkSplitter*/),
                           DBException,
                           ErrorCodes::InvalidOptions);
    };

    test(_nss2, Timestamp(42));
}

TEST_F(SplitChunkTest, SplitPointsOutOfRangeAtMinShouldFail) {
    auto test = [&](const NamespaceString& nss, const Timestamp& collTimestamp) {
        const auto collEpoch = OID::gen();

        ChunkType chunk;
        chunk.setCollectionUUID(UUID::gen());

        auto origVersion = ChunkVersion(1, 0, collEpoch, collTimestamp);
        chunk.setVersion(origVersion);
        chunk.setShard(ShardId(_shardName));

        auto chunkMin = BSON("a" << 1);
        auto chunkMax = BSON("a" << 10);
        chunk.setMin(chunkMin);
        chunk.setMax(chunkMax);

        std::vector<BSONObj> splitPoints{BSON("a" << 0), BSON("a" << 5)};

        setupCollection(nss, _keyPattern, {chunk});

        ASSERT_THROWS_CODE(ShardingCatalogManager::get(operationContext())
                               ->commitChunkSplit(operationContext(),
                                                  nss,
                                                  collEpoch,
                                                  collTimestamp,
                                                  ChunkRange(chunkMin, chunkMax),
                                                  splitPoints,
                                                  "shard0000",
                                                  false /* fromChunkSplitter*/),
                           DBException,
                           ErrorCodes::InvalidOptions);
    };

    test(_nss2, Timestamp(42));
}

TEST_F(SplitChunkTest, SplitPointsOutOfRangeAtMaxShouldFail) {
    auto test = [&](const NamespaceString& nss, const Timestamp& collTimestamp) {
        const auto collEpoch = OID::gen();

        ChunkType chunk;
        chunk.setName(OID::gen());
        chunk.setCollectionUUID(UUID::gen());

        auto origVersion = ChunkVersion(1, 0, collEpoch, collTimestamp);
        chunk.setVersion(origVersion);
        chunk.setShard(ShardId(_shardName));

        auto chunkMin = BSON("a" << 1);
        auto chunkMax = BSON("a" << 10);
        chunk.setMin(chunkMin);
        chunk.setMax(chunkMax);

        std::vector<BSONObj> splitPoints{BSON("a" << 5), BSON("a" << 15)};

        setupCollection(nss, _keyPattern, {chunk});

        ASSERT_THROWS_CODE(ShardingCatalogManager::get(operationContext())
                               ->commitChunkSplit(operationContext(),
                                                  nss,
                                                  collEpoch,
                                                  collTimestamp,
                                                  ChunkRange(chunkMin, chunkMax),
                                                  splitPoints,
                                                  "shard0000",
                                                  false /* fromChunkSplitter*/),
                           DBException,
                           ErrorCodes::InvalidOptions);
    };

    test(_nss2, Timestamp(42));
}

TEST_F(SplitChunkTest, SplitPointsWithDollarPrefixShouldFail) {
    auto test = [&](const NamespaceString& nss, const Timestamp& collTimestamp) {
        const auto collEpoch = OID::gen();

        ChunkType chunk;
        chunk.setCollectionUUID(UUID::gen());

        auto origVersion = ChunkVersion(1, 0, collEpoch, collTimestamp);
        chunk.setVersion(origVersion);
        chunk.setShard(ShardId(_shardName));

        auto chunkMin = BSON("a" << kMinBSONKey);
        auto chunkMax = BSON("a" << kMaxBSONKey);
        chunk.setMin(chunkMin);
        chunk.setMax(chunkMax);
        setupCollection(nss, _keyPattern, {chunk});

        ASSERT_THROWS(ShardingCatalogManager::get(operationContext())
                          ->commitChunkSplit(operationContext(),
                                             nss,
                                             collEpoch,
                                             collTimestamp,
                                             ChunkRange(chunkMin, chunkMax),
                                             {BSON("a" << BSON("$minKey" << 1))},
                                             "shard0000",
                                             false /* fromChunkSplitter*/),
                      DBException);
        ASSERT_THROWS(ShardingCatalogManager::get(operationContext())
                          ->commitChunkSplit(operationContext(),
                                             nss,
                                             collEpoch,
                                             collTimestamp,
                                             ChunkRange(chunkMin, chunkMax),
                                             {BSON("a" << BSON("$maxKey" << 1))},
                                             "shard0000",
                                             false /* fromChunkSplitter*/),
                      DBException);
    };

    test(_nss2, Timestamp(42));
}

TEST_F(SplitChunkTest, CantCommitSplitFromChunkSplitterDuringDefragmentation) {
    const auto& nss = _nss2;
    const auto collEpoch = OID::gen();
    const Timestamp collTimestamp{1, 0};
    const auto collUuid = UUID::gen();

    ChunkType chunk;
    chunk.setName(OID::gen());
    chunk.setCollectionUUID(collUuid);

    auto version = ChunkVersion(1, 0, collEpoch, collTimestamp);
    chunk.setVersion(version);
    chunk.setShard(ShardId(_shardName));

    auto chunkMin = BSON("a" << 1);
    auto chunkMax = BSON("a" << 10);
    chunk.setMin(chunkMin);
    chunk.setMax(chunkMax);

    auto chunkSplitPoint = BSON("a" << 5);
    std::vector<BSONObj> splitPoints{chunkSplitPoint};

    setupCollection(nss, _keyPattern, {chunk});

    // Bring collection in the `splitChunks` phase of the defragmentation
    DBDirectClient dbClient(operationContext());
    write_ops::UpdateCommandRequest updateOp(CollectionType::ConfigNS);
    updateOp.setUpdates({[&] {
        write_ops::UpdateOpEntry entry;
        entry.setQ(BSON(CollectionType::kUuidFieldName << collUuid));
        entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(
            BSON("$set" << BSON(CollectionType::kDefragmentCollectionFieldName << true))));
        return entry;
    }()});
    dbClient.update(updateOp);

    // The split commit must fail if the request is sent by the chunk splitter
    ASSERT_THROWS_CODE(ShardingCatalogManager::get(operationContext())
                           ->commitChunkSplit(operationContext(),
                                              nss,
                                              collEpoch,
                                              collTimestamp,
                                              ChunkRange(chunkMin, chunkMax),
                                              splitPoints,
                                              "shard0000",
                                              true /* fromChunkSplitter*/),
                       DBException,
                       ErrorCodes::ConflictingOperationInProgress);

    // The split commit must succeed if the request is sent by the defragmenter
    uassertStatusOK(ShardingCatalogManager::get(operationContext())
                        ->commitChunkSplit(operationContext(),
                                           nss,
                                           collEpoch,
                                           collTimestamp,
                                           ChunkRange(chunkMin, chunkMax),
                                           splitPoints,
                                           "shard0000",
                                           false /* fromChunkSplitter*/));
}

}  // namespace
}  // namespace mongo
