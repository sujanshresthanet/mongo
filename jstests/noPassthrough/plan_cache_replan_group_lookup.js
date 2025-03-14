/**
 * Test that plans with $group and $lookup lowered to SBE are cached and replanned as appropriate.
 * @tags: [
 *   requires_profiling,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");
load("jstests/libs/log.js");  // For findMatchingLogLine.
load("jstests/libs/profiler.js");
load("jstests/libs/sbe_util.js");      // For checkSBEEnabled.
load("jstests/libs/analyze_plan.js");  // For 'getAggPlanStages()'

const conn = MongoRunner.runMongod();
const db = conn.getDB("test");
const coll = db.plan_cache_replan_group_lookup;
const foreignCollName = "foreign";
coll.drop();

function getPlansForCacheEntry(match) {
    const matchingCacheEntries = coll.getPlanCache().list([{$match: match}]);
    assert.eq(matchingCacheEntries.length, 1, coll.getPlanCache().list());
    return matchingCacheEntries[0];
}

function planHasIxScanStageForKey(planStats, keyPattern) {
    const stage = getPlanStage(planStats, "IXSCAN");
    if (stage === null) {
        return false;
    }

    return bsonWoCompare(keyPattern, stage.keyPattern) === 0;
}

function assertCacheUsage(
    multiPlanning, cacheEntryIsActive, cachedIndex, pipeline, aggOptions = {}) {
    const profileObj = getLatestProfilerEntry(db, {op: "command", ns: coll.getFullName()});
    const queryHash = profileObj.queryHash;
    const planCacheKey = profileObj.planCacheKey;
    assert.eq(multiPlanning, !!profileObj.fromMultiPlanner);

    const entry = getPlansForCacheEntry({queryHash: queryHash});
    // TODO(SERVER-61507): Convert the assertion to SBE cache once lowered $lookup integrates
    // with SBE plan cache.
    assert.eq(entry.version, 1);
    assert.eq(cacheEntryIsActive, entry.isActive);

    // If the entry is active, we should have a plan cache key.
    if (entry.isActive) {
        assert(entry.planCacheKey);
    }
    if (planCacheKey) {
        assert.eq(entry.planCacheKey, planCacheKey);
        const explain = coll.explain().aggregate(pipeline, aggOptions);
        const explainKey = explain.hasOwnProperty("queryPlanner")
            ? explain.queryPlanner.planCacheKey
            : explain.stages[0].$cursor.queryPlanner.planCacheKey;
        assert.eq(explainKey, entry.planCacheKey);
    }
    assert.eq(planHasIxScanStageForKey(getCachedPlan(entry.cachedPlan), cachedIndex), true, entry);
}

assert.commandWorked(db.setProfilingLevel(2));

// Carefully construct a collection so that some queries will do well with an {a: 1} index
// and others with a {b: 1} index.
for (let i = 1000; i < 1100; i++) {
    assert.commandWorked(coll.insert({a: 1, b: i, c: 5}));
    assert.commandWorked(coll.insert({a: 1, b: i, c: 6}));
    assert.commandWorked(coll.insert({a: 1, b: i, c: 7}));
}
for (let i = 1000; i < 1100; i++) {
    assert.commandWorked(coll.insert({a: i, b: 1, c: 5}));
    assert.commandWorked(coll.insert({a: i, b: 1, c: 8}));
    assert.commandWorked(coll.insert({a: i, b: 1, c: 8}));
}
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({b: 1}));

function setUpActiveCacheEntry(pipeline, cachedIndex) {
    // For the first run, the query should go through multiplanning and create inactive cache entry.
    assert.eq(2, coll.aggregate(pipeline).toArray()[0].n);
    assertCacheUsage(true /*multiPlanning*/, false /*cacheEntryIsActive*/, cachedIndex, pipeline);

    // After the second run, the inactive cache entry should be promoted to an active entry.
    assert.eq(2, coll.aggregate(pipeline).toArray()[0].n);
    assertCacheUsage(true /*multiPlanning*/, true /*cacheEntryIsActive*/, cachedIndex, pipeline);

    // For the third run, the active cached query should be used.
    assert.eq(2, coll.aggregate(pipeline).toArray()[0].n);
    assertCacheUsage(false /*multiPlanning*/, true /*cacheEntryIsActive*/, cachedIndex, pipeline);
}

function testFn(aIndexPipeline,
                bIndexPipeline,
                setUpFn = undefined,
                tearDownFn = undefined,
                explainFn = undefined) {
    if (setUpFn) {
        setUpFn();
    }

    if (explainFn) {
        explainFn(aIndexPipeline);
        explainFn(bIndexPipeline);
    }

    setUpActiveCacheEntry(aIndexPipeline, {a: 1} /* cachedIndex */);

    // Now run the other pipeline, which has the same query shape but is faster with a different
    // index. It should trigger re-planning of the query.
    assert.eq(3, coll.aggregate(bIndexPipeline).toArray()[0].n);

    // The other pipeline again, The cache should be used now.
    assertCacheUsage(true /*multiPlanning*/,
                     true /*cacheEntryIsActive*/,
                     {b: 1} /*cachedIndex*/,
                     bIndexPipeline);

    // Run it once again so that the cache entry is reused.
    assert.eq(3, coll.aggregate(bIndexPipeline).toArray()[0].n);
    assertCacheUsage(false /*multiPlanning*/,
                     true /*cacheEntryIsActive*/,
                     {b: 1} /*cachedIndex*/,
                     bIndexPipeline);

    if (tearDownFn) {
        tearDownFn();
    }

    coll.getPlanCache().clear();
}

// This pipeline will be quick with {a: 1} index, and far slower {b: 1} index. With the {a: 1}
// index, the server should only need to examine one document. Using {b: 1}, it will have to
// scan through each document which has 2 as the value of the 'b' field.
const aIndexPredicate = [{$match: {a: 1042, b: 1}}];

// Opposite of 'aIndexQuery'. Should be quick if the {b: 1} index is used, and slower if the
// {a: 1} index is used.
const bIndexPredicate = [{$match: {a: 1, b: 1042}}];

// $group tests.
const groupSuffix = [{$group: {_id: "$c"}}, {$count: "n"}];
testFn(aIndexPredicate.concat(groupSuffix), bIndexPredicate.concat(groupSuffix));

// $lookup tests.
const lookupStage =
    [{$lookup: {from: foreignCollName, localField: 'c', foreignField: 'foreignKey', as: 'out'}}];
const aLookup = aIndexPredicate.concat(lookupStage).concat(groupSuffix);
const bLookup = bIndexPredicate.concat(lookupStage).concat(groupSuffix);

function createLookupForeignColl() {
    const foreignColl = db[foreignCollName];

    // Here, the values for 'foreignKey' are expected to match existing values for 'c' in 'coll'.
    assert.commandWorked(foreignColl.insert([{foreignKey: 8}, {foreignKey: 6}]));
}

function dropLookupForeignColl() {
    assert(db[foreignCollName].drop());
}

const lookupPushdownEnabled = checkSBEEnabled(db);
const lookupPushdownNLJEnabled = checkSBEEnabled(db, ["featureFlagSbeFull"]);
function verifyCorrectLookupAlgorithmUsed(targetJoinAlgorithm, pipeline, aggOptions = {}) {
    if (!lookupPushdownEnabled) {
        return;
    }

    if (!lookupPushdownNLJEnabled && targetJoinAlgorithm === "NestedLoopJoin") {
        targetJoinAlgorithm = "Classic";
    }

    const explain = coll.explain().aggregate(pipeline, aggOptions);
    const eqLookupNodes = getAggPlanStages(explain, "EQ_LOOKUP");

    if (targetJoinAlgorithm === "Classic") {
        assert.eq(eqLookupNodes.length, 0, "expected no EQ_LOOKUP nodes; got " + tojson(explain));
    } else {
        // Verify via explain that $lookup was lowered and appropriate $lookup algorithm was chosen.
        assert.eq(eqLookupNodes.length,
                  1,
                  "expected at least one EQ_LOOKUP node; got " + tojson(explain));
        assert.eq(eqLookupNodes[0].strategy, targetJoinAlgorithm);
    }
}

// NLJ.
testFn(aLookup,
       bLookup,
       createLookupForeignColl,
       dropLookupForeignColl,
       (pipeline) =>
           verifyCorrectLookupAlgorithmUsed("NestedLoopJoin", pipeline, {allowDiskUse: false}));

// INLJ.
testFn(aLookup,
       bLookup,
       () => {
           createLookupForeignColl();
           assert.commandWorked(db[foreignCollName].createIndex({foreignKey: 1}));
       },
       dropLookupForeignColl,
       (pipeline) =>
           verifyCorrectLookupAlgorithmUsed("IndexedLoopJoin", pipeline, {allowDiskUse: false}));

// HJ.
testFn(aLookup, bLookup, () => {
    createLookupForeignColl();
}, dropLookupForeignColl, (pipeline) => verifyCorrectLookupAlgorithmUsed("HashJoin", pipeline, {
                              allowDiskUse: true
                          }));

// Verify that a cached plan which initially uses an INLJ will use HJ once the index is dropped and
// the foreign collection is dropped, and NLJ when 'allowDiskUse' is set to 'false'.

// For the first run, the query should go through multiplanning and create inactive cache entry.
createLookupForeignColl();
assert.commandWorked(db[foreignCollName].createIndex({foreignKey: 1}));
verifyCorrectLookupAlgorithmUsed("IndexedLoopJoin", aLookup, {allowDiskUse: true});
setUpActiveCacheEntry(aLookup, {a: 1} /* cachedIndex */);

// Drop the index. This should result in using the active plan, but switching to HJ.
assert.commandWorked(db[foreignCollName].dropIndex({foreignKey: 1}));
verifyCorrectLookupAlgorithmUsed("HashJoin", aLookup, {allowDiskUse: true});
assert.eq(2, coll.aggregate(aLookup).toArray()[0].n);
assertCacheUsage(
    false /*multiPlanning*/, true /*cacheEntryIsActive*/, {a: 1} /*cachedIndex*/, aLookup);

// Set 'allowDiskUse' to 'false'. This should still result in using the active plan, but switching
// to NLJ.
verifyCorrectLookupAlgorithmUsed("NestedLoopJoin", aLookup, {allowDiskUse: false});
assert.eq(2, coll.aggregate(aLookup).toArray()[0].n);
assertCacheUsage(
    false /*multiPlanning*/, true /*cacheEntryIsActive*/, {a: 1} /*cachedIndex*/, aLookup);

// Drop the foreign collection. This should still result in using the active plan with a special
// empty collection plan.
dropLookupForeignColl();
verifyCorrectLookupAlgorithmUsed("NonExistentForeignCollection", aLookup, {allowDiskUse: true});
assert.eq(2, coll.aggregate(aLookup).toArray()[0].n);
assertCacheUsage(
    false /*multiPlanning*/, true /*cacheEntryIsActive*/, {a: 1} /*cachedIndex*/, aLookup);

// Verify that changing the plan for the right side does not trigger a replan.
const foreignColl = db[foreignCollName];
coll.drop();
foreignColl.drop();
coll.getPlanCache().clear();

assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({b: 1}));
assert.commandWorked(coll.insert([
    {_id: 0, a: 1, b: 1},
    {_id: 1, a: 1, b: 2},
    {_id: 2, a: 1, b: 3},
    {_id: 3, a: 1, b: 4},
]));

assert.commandWorked(foreignColl.createIndex({c: 1}));
for (let i = -30; i < 30; ++i) {
    assert.commandWorked(foreignColl.insert({_id: i, c: i}));
}

const avoidReplanLookupPipeline = [
    {$match: {a: 1, b: 3}},
    {$lookup: {from: foreignColl.getName(), as: "as", localField: "a", foreignField: "c"}}
];
function runLookupQuery(options = {}) {
    assert.eq([{_id: 2, a: 1, b: 3, as: [{_id: 1, c: 1}]}],
              coll.aggregate(avoidReplanLookupPipeline, options).toArray());
}

// Verify that we are using IndexedLoopJoin.
verifyCorrectLookupAlgorithmUsed(
    "IndexedLoopJoin", avoidReplanLookupPipeline, {allowDiskUse: false});

runLookupQuery({allowDiskUse: false});
assertCacheUsage(true /*multiPlanning*/,
                 false /*activeCacheEntry*/,
                 {b: 1} /*cachedIndex*/,
                 avoidReplanLookupPipeline,
                 {allowDiskUse: false});

runLookupQuery({allowDiskUse: false});
assertCacheUsage(true /*multiPlanning*/,
                 true /*activeCacheEntry*/,
                 {b: 1} /*cachedIndex*/,
                 avoidReplanLookupPipeline,
                 {allowDiskUse: false});

// After dropping the index on the right-hand side, we should NOT replan the cached query. We
// will, however, choose a different join algorithm.
assert.commandWorked(foreignColl.dropIndex({c: 1}));

// Verify that we are now using NestedLoopJoin.
verifyCorrectLookupAlgorithmUsed(
    "NestedLoopJoin", avoidReplanLookupPipeline, {allowDiskUse: false});

runLookupQuery({allowDiskUse: false});
assertCacheUsage(false /*multiPlanning*/,
                 true /*activeCacheEntry*/,
                 {b: 1} /*cachedIndex*/,
                 avoidReplanLookupPipeline,
                 {allowDiskUse: false});

runLookupQuery({allowDiskUse: false});
assertCacheUsage(false /*multiPlanning*/,
                 true /*activeCacheEntry*/,
                 {b: 1} /*cachedIndex*/,
                 avoidReplanLookupPipeline,
                 {allowDiskUse: false});

// Run with 'allowDiskUse: true'. This should now use HashJoin, and we should still avoid
// replanning the cached query.
verifyCorrectLookupAlgorithmUsed("HashJoin", avoidReplanLookupPipeline, {allowDiskUse: true});

runLookupQuery({allowDiskUse: true});
assertCacheUsage(false /*multiPlanning*/,
                 true /*activeCacheEntry*/,
                 {b: 1} /*cachedIndex*/,
                 avoidReplanLookupPipeline,
                 {allowDiskUse: true});
runLookupQuery({allowDiskUse: true});
assertCacheUsage(false /*multiPlanning*/,
                 true /*activeCacheEntry*/,
                 {b: 1} /*cachedIndex*/,
                 avoidReplanLookupPipeline,
                 {allowDiskUse: true});

// Verify that disabling $lookup pushdown into SBE does not trigger a replan, and uses the
// correct engine to execute results.
coll.getPlanCache().clear();
assert.commandWorked(foreignColl.createIndex({c: 1}));

const avoidReplanGroupPipeline = [
    {$match: {a: 1, b: 3}},
    {$group: {_id: "$a", out: {"$sum": 1}}},
];

function runGroupQuery() {
    assert.eq([{_id: 1, out: 1}], coll.aggregate(avoidReplanGroupPipeline).toArray());
}

// Verify that we are using IndexedLoopJoin.
verifyCorrectLookupAlgorithmUsed("IndexedLoopJoin", avoidReplanLookupPipeline);

// Set up an active cache entry.
runLookupQuery();
assertCacheUsage(true /*multiPlanning*/,
                 false /*activeCacheEntry*/,
                 {b: 1} /*cachedIndex*/,
                 avoidReplanLookupPipeline);
runLookupQuery();
assertCacheUsage(true /*multiPlanning*/,
                 true /*activeCacheEntry*/,
                 {b: 1} /*cachedIndex*/,
                 avoidReplanLookupPipeline);
runLookupQuery();
assertCacheUsage(false /*multiPlanning*/,
                 true /*activeCacheEntry*/,
                 {b: 1} /*cachedIndex*/,
                 avoidReplanLookupPipeline);
runLookupQuery();
assertCacheUsage(false /*multiPlanning*/,
                 true /*activeCacheEntry*/,
                 {b: 1} /*cachedIndex*/,
                 avoidReplanLookupPipeline);

// Disable $lookup pushdown. This should not invalidate the cache entry, but it should prevent
// $lookup from being pushed down.
assert.commandWorked(
    db.adminCommand({setParameter: 1, internalQuerySlotBasedExecutionDisableLookupPushdown: true}));

// Verify via explain that $lookup was NOT lowered.
let explain = coll.explain().aggregate(avoidReplanLookupPipeline);
const eqLookupNodes = getAggPlanStages(explain, "EQ_LOOKUP");
assert.eq(eqLookupNodes.length, 0, "expected no EQ_LOOKUP nodes; got " + tojson(explain));

if (checkSBEEnabled(db, ["featureFlagSbePlanCache"])) {
    runLookupQuery();
    const profileObj = getLatestProfilerEntry(db, {op: "command", ns: coll.getFullName()});
    const matchingCacheEntries =
        coll.getPlanCache().list([{$match: {queryHash: profileObj.queryHash}}]);
    assert.eq(1, matchingCacheEntries.length);
} else {
    // When the SBE plan cache is disabled, we will be able to reuse the same cache entry.
    runLookupQuery();
    assertCacheUsage(false /*multiPlanning*/,
                     true /*activeCacheEntry*/,
                     {b: 1} /*cachedIndex*/,
                     avoidReplanLookupPipeline);
    runLookupQuery();
    assertCacheUsage(false /*multiPlanning*/,
                     true /*activeCacheEntry*/,
                     {b: 1} /*cachedIndex*/,
                     avoidReplanLookupPipeline);
}

// Verify that disabling $group pushdown into SBE does not trigger a replan, and uses the
// correct engine to execute results.
coll.getPlanCache().clear();

// Verify that $group gets pushed down, provided that SBE is enabled.
let groupNodes;
if (checkSBEEnabled(db)) {
    explain = coll.explain().aggregate(avoidReplanGroupPipeline);
    let groupNodes = getAggPlanStages(explain, "GROUP");
    assert.eq(groupNodes.length, 1);
}

// Set up an active cache entry.
runGroupQuery();
assertCacheUsage(true /*multiPlanning*/,
                 false /*activeCacheEntry*/,
                 {b: 1} /*cachedIndex*/,
                 avoidReplanGroupPipeline);
runGroupQuery();
assertCacheUsage(true /*multiPlanning*/,
                 true /*activeCacheEntry*/,
                 {b: 1} /*cachedIndex*/,
                 avoidReplanGroupPipeline);
runGroupQuery();
assertCacheUsage(false /*multiPlanning*/,
                 true /*activeCacheEntry*/,
                 {b: 1} /*cachedIndex*/,
                 avoidReplanGroupPipeline);
runGroupQuery();
assertCacheUsage(false /*multiPlanning*/,
                 true /*activeCacheEntry*/,
                 {b: 1} /*cachedIndex*/,
                 avoidReplanGroupPipeline);

// Disable $group pushdown. This should not invalidate the cache entry, but it should prevent $group
// from being pushed down.
assert.commandWorked(
    db.adminCommand({setParameter: 1, internalQuerySlotBasedExecutionDisableGroupPushdown: true}));

explain = coll.explain().aggregate(avoidReplanLookupPipeline);
groupNodes = getAggPlanStages(explain, "GROUP");
assert.eq(groupNodes.length, 0);

if (checkSBEEnabled(db, ["featureFlagSbePlanCache"])) {
    runGroupQuery();
    const profileObj = getLatestProfilerEntry(db, {op: "command", ns: coll.getFullName()});
    const matchingCacheEntries =
        coll.getPlanCache().list([{$match: {queryHash: profileObj.queryHash}}]);
    assert.eq(1, matchingCacheEntries.length);
} else {
    // When the SBE plan cache is disabled, we will be able to reuse the same cache entry.
    runGroupQuery();
    assertCacheUsage(false /*multiPlanning*/,
                     true /*activeCacheEntry*/,
                     {b: 1} /*cachedIndex*/,
                     avoidReplanGroupPipeline);
    runGroupQuery();
    assertCacheUsage(false /*multiPlanning*/,
                     true /*activeCacheEntry*/,
                     {b: 1} /*cachedIndex*/,
                     avoidReplanGroupPipeline);
}

MongoRunner.stopMongod(conn);
}());
