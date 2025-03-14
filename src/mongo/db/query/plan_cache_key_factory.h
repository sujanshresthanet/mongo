/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#pragma once

#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/classic_plan_cache.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/query/sbe_plan_cache.h"

namespace mongo {
namespace plan_cache_detail {
/**
 * Serializes indexability discriminators, appending them to keyBuilder. This function is used
 * during the computation of a query's plan cache key to ensure that two queries with different
 * index eligibilities will have different cache keys.
 */
void encodeIndexability(const MatchExpression* tree,
                        const PlanCacheIndexabilityState& indexabilityState,
                        StringBuilder* keyBuilder);

/**
 * A dispatch tag for the factory functions below.
 */
template <typename KeyType>
struct PlanCacheKeyTag {};

/**
 * Creates a key for the classic plan cache from the canonical query and collection instances.
 */
PlanCacheKey make(const CanonicalQuery& query,
                  const CollectionPtr& collection,
                  PlanCacheKeyTag<PlanCacheKey> tag);

/**
 * Creates a key for the SBE plan cache from the canonical query and collection instances.
 */
sbe::PlanCacheKey make(const CanonicalQuery& query,
                       const CollectionPtr& collection,
                       PlanCacheKeyTag<sbe::PlanCacheKey> tag);
}  // namespace plan_cache_detail

namespace plan_cache_key_factory {
/**
 * A factory helper to make a plan cache key of the given type.
 *
 * Note: when requesting an SBE plan cache key, callers must have already established the shard
 * version on the collection (if this is a sharded cluster).
 */
template <typename Key>
Key make(const CanonicalQuery& query, const CollectionPtr& collection) {
    return plan_cache_detail::make(query, collection, plan_cache_detail::PlanCacheKeyTag<Key>{});
}
}  // namespace plan_cache_key_factory
}  // namespace mongo
