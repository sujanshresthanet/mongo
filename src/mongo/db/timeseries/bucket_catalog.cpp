/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/timeseries/bucket_catalog.h"

#include <algorithm>
#include <boost/iterator/transform_iterator.hpp>

#include "mongo/bson/util/bsoncolumn.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/timeseries/bucket_catalog_helpers.h"
#include "mongo/db/timeseries/bucket_compression.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/logv2/redaction.h"
#include "mongo/platform/compiler.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/fail_point.h"

namespace mongo {
namespace {

void normalizeArray(BSONArrayBuilder* builder, const BSONObj& obj);
void normalizeObject(BSONObjBuilder* builder, const BSONObj& obj);

const auto getBucketCatalog = ServiceContext::declareDecoration<BucketCatalog>();
MONGO_FAIL_POINT_DEFINE(hangTimeseriesDirectModificationBeforeWriteConflict);

uint8_t numDigits(uint32_t num) {
    uint8_t numDigits = 0;
    while (num) {
        num /= 10;
        ++numDigits;
    }
    return numDigits;
}

void normalizeArray(BSONArrayBuilder* builder, const BSONObj& obj) {
    for (auto& arrayElem : obj) {
        if (arrayElem.type() == BSONType::Array) {
            BSONArrayBuilder subArray = builder->subarrayStart();
            normalizeArray(&subArray, arrayElem.Obj());
        } else if (arrayElem.type() == BSONType::Object) {
            BSONObjBuilder subObject = builder->subobjStart();
            normalizeObject(&subObject, arrayElem.Obj());
        } else {
            builder->append(arrayElem);
        }
    }
}

void normalizeObject(BSONObjBuilder* builder, const BSONObj& obj) {
    // BSONObjIteratorSorted provides an abstraction similar to what this function does. However it
    // is using a lexical comparison that is slower than just doing a binary comparison of the field
    // names. That is all we need here as we are looking to create something that is binary
    // comparable no matter of field order provided by the user.

    // Helper that extracts the necessary data from a BSONElement that we can sort and re-construct
    // the same BSONElement from.
    struct Field {
        BSONElement element() const {
            return BSONElement(fieldName.rawData() - 1,  // Include type byte before field name
                               fieldName.size() + 1,     // Include null terminator after field name
                               totalSize);
        }
        bool operator<(const Field& rhs) const {
            return fieldName < rhs.fieldName;
        }
        StringData fieldName;
        int totalSize;
    };

    // Put all elements in a buffer, sort it and then continue normalize in sorted order
    auto num = obj.nFields();
    static constexpr std::size_t kNumStaticFields = 16;
    boost::container::small_vector<Field, kNumStaticFields> fields;
    fields.resize(num);
    BSONObjIterator bsonIt(obj);
    int i = 0;
    while (bsonIt.more()) {
        auto elem = bsonIt.next();
        fields[i++] = {elem.fieldNameStringData(), elem.size()};
    }
    auto it = fields.begin();
    auto end = fields.end();
    std::sort(it, end);
    for (; it != end; ++it) {
        auto elem = it->element();
        if (elem.type() == BSONType::Array) {
            BSONArrayBuilder subArray(builder->subarrayStart(elem.fieldNameStringData()));
            normalizeArray(&subArray, elem.Obj());
        } else if (elem.type() == BSONType::Object) {
            BSONObjBuilder subObject(builder->subobjStart(elem.fieldNameStringData()));
            normalizeObject(&subObject, elem.Obj());
        } else {
            builder->append(elem);
        }
    }
}

void normalizeTopLevel(BSONObjBuilder* builder, const BSONElement& elem) {
    if (elem.type() == BSONType::Array) {
        BSONArrayBuilder subArray(builder->subarrayStart(elem.fieldNameStringData()));
        normalizeArray(&subArray, elem.Obj());
    } else if (elem.type() == BSONType::Object) {
        BSONObjBuilder subObject(builder->subobjStart(elem.fieldNameStringData()));
        normalizeObject(&subObject, elem.Obj());
    } else {
        builder->append(elem);
    }
}

OperationId getOpId(OperationContext* opCtx,
                    BucketCatalog::CombineWithInsertsFromOtherClients combine) {
    switch (combine) {
        case BucketCatalog::CombineWithInsertsFromOtherClients::kAllow:
            return 0;
        case BucketCatalog::CombineWithInsertsFromOtherClients::kDisallow:
            invariant(opCtx->getOpID());
            return opCtx->getOpID();
    }
    MONGO_UNREACHABLE;
}

BSONObj buildControlMinTimestampDoc(StringData timeField, Date_t roundedTime) {
    BSONObjBuilder builder;
    builder.append(timeField, roundedTime);
    return builder.obj();
}

std::pair<OID, Date_t> generateBucketId(const Date_t& time, const TimeseriesOptions& options) {
    OID bucketId = OID::gen();

    // We round the measurement timestamp down to the nearest minute, hour, or day depending on the
    // granularity. We do this for two reasons. The first is so that if measurements come in
    // slightly out of order, we don't have to close the current bucket due to going backwards in
    // time. The second, and more important reason, is so that we reliably group measurements
    // together into predictable chunks for sharding. This way we know from a measurement timestamp
    // what the bucket timestamp will be, so we can route measurements to the right shard chunk.
    auto roundedTime = timeseries::roundTimestampToGranularity(time, options.getGranularity());
    int64_t const roundedSeconds = durationCount<Seconds>(roundedTime.toDurationSinceEpoch());
    bucketId.setTimestamp(roundedSeconds);

    // Now, if we stopped here we could end up with bucket OID collisions. Consider the case where
    // we have the granularity set to 'Hours'. This means we will round down to the nearest day, so
    // any bucket generated on the same machine on the same day will have the same timestamp portion
    // and unique instance portion of the OID. Only the increment will differ. Since we only use 3
    // bytes for the increment portion, we run a serious risk of overflow if we are generating lots
    // of buckets.
    //
    // To address this, we'll take the difference between the actual timestamp and the rounded
    // timestamp and add it to the instance portion of the OID to ensure we can't have a collision.
    // for timestamps generated on the same machine.
    //
    // This leaves open the possibility that in the case of step-down/step-up, we could get a
    // collision if the old primary and the new primary have unique instance bits that differ by
    // less than the maximum rounding difference. This is quite unlikely though, and can be resolved
    // by restarting the new primary. It remains an open question whether we can fix this in a
    // better way.
    // TODO (SERVER-61412): Avoid time-series bucket OID collisions after election
    auto instance = bucketId.getInstanceUnique();
    uint32_t sum = DataView(reinterpret_cast<char*>(instance.bytes)).read<uint32_t>(1) +
        (durationCount<Seconds>(time.toDurationSinceEpoch()) - roundedSeconds);
    DataView(reinterpret_cast<char*>(instance.bytes)).write<uint32_t>(sum, 1);
    bucketId.setInstanceUnique(instance);

    return {bucketId, roundedTime};
}

Status getTimeseriesBucketClearedError(const OID& bucketId,
                                       const boost::optional<NamespaceString>& ns = boost::none) {
    std::string nsIdentification;
    if (ns) {
        nsIdentification.assign(str::stream() << " for namespace " << *ns);
    }
    return {ErrorCodes::TimeseriesBucketCleared,
            str::stream() << "Time-series bucket " << bucketId << nsIdentification
                          << " was cleared"};
}
}  // namespace

void BucketCatalog::ExecutionStatsController::incNumBucketInserts(long long increment) {
    _collectionStats->numBucketInserts.fetchAndAddRelaxed(increment);
    _globalStats->numBucketInserts.fetchAndAddRelaxed(increment);
}

void BucketCatalog::ExecutionStatsController::incNumBucketUpdates(long long increment) {
    _collectionStats->numBucketUpdates.fetchAndAddRelaxed(increment);
    _globalStats->numBucketUpdates.fetchAndAddRelaxed(increment);
}

void BucketCatalog::ExecutionStatsController::incNumBucketsOpenedDueToMetadata(
    long long increment) {
    _collectionStats->numBucketsOpenedDueToMetadata.fetchAndAddRelaxed(increment);
    _globalStats->numBucketsOpenedDueToMetadata.fetchAndAddRelaxed(increment);
}

void BucketCatalog::ExecutionStatsController::incNumBucketsClosedDueToCount(long long increment) {
    _collectionStats->numBucketsClosedDueToCount.fetchAndAddRelaxed(increment);
    _globalStats->numBucketsClosedDueToCount.fetchAndAddRelaxed(increment);
}

void BucketCatalog::ExecutionStatsController::incNumBucketsClosedDueToSchemaChange(
    long long increment) {
    _collectionStats->numBucketsClosedDueToSchemaChange.fetchAndAddRelaxed(increment);
    _globalStats->numBucketsClosedDueToSchemaChange.fetchAndAddRelaxed(increment);
}

void BucketCatalog::ExecutionStatsController::incNumBucketsClosedDueToSize(long long increment) {
    _collectionStats->numBucketsClosedDueToSize.fetchAndAddRelaxed(increment);
    _globalStats->numBucketsClosedDueToSize.fetchAndAddRelaxed(increment);
}

void BucketCatalog::ExecutionStatsController::incNumBucketsClosedDueToTimeForward(
    long long increment) {
    _collectionStats->numBucketsClosedDueToTimeForward.fetchAndAddRelaxed(increment);
    _globalStats->numBucketsClosedDueToTimeForward.fetchAndAddRelaxed(increment);
}

void BucketCatalog::ExecutionStatsController::incNumBucketsClosedDueToTimeBackward(
    long long increment) {
    _collectionStats->numBucketsClosedDueToTimeBackward.fetchAndAddRelaxed(increment);
    _globalStats->numBucketsClosedDueToTimeBackward.fetchAndAddRelaxed(increment);
}

void BucketCatalog::ExecutionStatsController::incNumBucketsClosedDueToMemoryThreshold(
    long long increment) {
    _collectionStats->numBucketsClosedDueToMemoryThreshold.fetchAndAddRelaxed(increment);
    _globalStats->numBucketsClosedDueToMemoryThreshold.fetchAndAddRelaxed(increment);
}

void BucketCatalog::ExecutionStatsController::incNumBucketsArchivedDueToTimeForward(
    long long increment) {
    _collectionStats->numBucketsArchivedDueToTimeForward.fetchAndAddRelaxed(increment);
    _globalStats->numBucketsArchivedDueToTimeForward.fetchAndAddRelaxed(increment);
}

void BucketCatalog::ExecutionStatsController::incNumBucketsArchivedDueToTimeBackward(
    long long increment) {
    _collectionStats->numBucketsArchivedDueToTimeBackward.fetchAndAddRelaxed(increment);
    _globalStats->numBucketsArchivedDueToTimeBackward.fetchAndAddRelaxed(increment);
}

void BucketCatalog::ExecutionStatsController::incNumBucketsArchivedDueToMemoryThreshold(
    long long increment) {
    _collectionStats->numBucketsArchivedDueToMemoryThreshold.fetchAndAddRelaxed(increment);
    _globalStats->numBucketsArchivedDueToMemoryThreshold.fetchAndAddRelaxed(increment);
}

void BucketCatalog::ExecutionStatsController::incNumCommits(long long increment) {
    _collectionStats->numCommits.fetchAndAddRelaxed(increment);
    _globalStats->numCommits.fetchAndAddRelaxed(increment);
}

void BucketCatalog::ExecutionStatsController::incNumWaits(long long increment) {
    _collectionStats->numWaits.fetchAndAddRelaxed(increment);
    _globalStats->numWaits.fetchAndAddRelaxed(increment);
}

void BucketCatalog::ExecutionStatsController::incNumMeasurementsCommitted(long long increment) {
    _collectionStats->numMeasurementsCommitted.fetchAndAddRelaxed(increment);
    _globalStats->numMeasurementsCommitted.fetchAndAddRelaxed(increment);
}

void BucketCatalog::ExecutionStatsController::incNumBucketsReopened(long long increment) {
    _collectionStats->numBucketsReopened.fetchAndAddRelaxed(increment);
    _globalStats->numBucketsReopened.fetchAndAddRelaxed(increment);
}

void BucketCatalog::ExecutionStatsController::incNumBucketsKeptOpenDueToLargeMeasurements(
    long long increment) {
    _collectionStats->numBucketsKeptOpenDueToLargeMeasurements.fetchAndAddRelaxed(increment);
    _globalStats->numBucketsKeptOpenDueToLargeMeasurements.fetchAndAddRelaxed(increment);
}

class BucketCatalog::Bucket {
public:
    friend class BucketCatalog;

    Bucket(const OID& id, StripeNumber stripe, BucketKey::Hash hash)
        : _id(id), _stripe(stripe), _keyHash(hash) {}

    /**
     * Returns the ID for the underlying bucket.
     */
    const OID& id() const {
        return _id;
    }

    /**
     * Returns the number of the stripe that owns the bucket
     */
    StripeNumber stripe() const {
        return _stripe;
    }

    /**
     * Returns the pre-computed hash of the corresponding BucketKey
     */
    BucketKey::Hash keyHash() const {
        return _keyHash;
    }

    // Returns the time associated with the bucket (id)
    Date_t getTime() const {
        return _minTime;
    }

    /**
     * Returns the timefield for the underlying bucket.
     */
    StringData getTimeField() {
        return _timeField;
    }

    /**
     * Returns whether all measurements have been committed.
     */
    bool allCommitted() const {
        return _batches.empty() && !_preparedBatch;
    }

    /**
     * Returns total number of measurements in the bucket.
     */
    uint32_t numMeasurements() const {
        return _numMeasurements;
    }

    /**
     * Determines if the schema for an incoming measurement is incompatible with those already
     * stored in the bucket.
     *
     * Returns true if incompatible
     */
    bool schemaIncompatible(const BSONObj& input,
                            boost::optional<StringData> metaField,
                            const StringData::ComparatorInterface* comparator) {
        auto result = _schema.update(input, metaField, comparator);
        return (result == timeseries::Schema::UpdateStatus::Failed);
    }

private:
    /**
     * Determines the effect of adding 'doc' to this bucket. If adding 'doc' causes this bucket
     * to overflow, we will create a new bucket and recalculate the change to the bucket size
     * and data fields.
     */
    void _calculateBucketFieldsAndSizeChange(const BSONObj& doc,
                                             boost::optional<StringData> metaField,
                                             NewFieldNames* newFieldNamesToBeInserted,
                                             uint32_t* sizeToBeAdded) const {
        // BSON size for an object with an empty object field where field name is empty string.
        // We can use this as an offset to know the size when we have real field names.
        static constexpr int emptyObjSize = 12;
        // Validate in debug builds that this size is correct
        dassert(emptyObjSize == BSON("" << BSONObj()).objsize());

        newFieldNamesToBeInserted->clear();
        *sizeToBeAdded = 0;
        auto numMeasurementsFieldLength = numDigits(_numMeasurements);
        for (const auto& elem : doc) {
            auto fieldName = elem.fieldNameStringData();
            if (fieldName == metaField) {
                // Ignore the metadata field since it will not be inserted.
                continue;
            }

            auto hashedKey = StringSet::hasher().hashed_key(fieldName);
            if (!_fieldNames.contains(hashedKey)) {
                // Record the new field name only if it hasn't been committed yet. There could be
                // concurrent batches writing to this bucket with the same new field name, but
                // they're not guaranteed to commit successfully.
                newFieldNamesToBeInserted->push_back(hashedKey);

                // Only update the bucket size once to account for the new field name if it isn't
                // already pending a commit from another batch.
                if (!_uncommittedFieldNames.contains(hashedKey)) {
                    // Add the size of an empty object with that field name.
                    *sizeToBeAdded += emptyObjSize + fieldName.size();

                    // The control.min and control.max summaries don't have any information for this
                    // new field name yet. Add two measurements worth of data to account for this.
                    // As this is the first measurement for this field, min == max.
                    *sizeToBeAdded += elem.size() * 2;
                }
            }

            // Add the element size, taking into account that the name will be changed to its
            // positional number. Add 1 to the calculation since the element's field name size
            // accounts for a null terminator whereas the stringified position does not.
            *sizeToBeAdded += elem.size() - elem.fieldNameSize() + numMeasurementsFieldLength + 1;
        }
    }

    /**
     * Returns whether BucketCatalog::commit has been called at least once on this bucket.
     */
    bool _hasBeenCommitted() const {
        return _numCommittedMeasurements != 0 || _preparedBatch;
    }

    /**
     * Return a pointer to the current, open batch.
     */
    std::shared_ptr<WriteBatch> _activeBatch(OperationId opId, ExecutionStatsController& stats) {
        auto it = _batches.find(opId);
        if (it == _batches.end()) {
            it =
                _batches
                    .try_emplace(
                        opId, std::make_shared<WriteBatch>(BucketHandle{_id, _stripe}, opId, stats))
                    .first;
        }
        return it->second;
    }

    // The bucket ID for the underlying document
    const OID _id;

    // The stripe which owns this bucket.
    const StripeNumber _stripe;

    // The pre-computed hash of the associated BucketKey
    const BucketKey::Hash _keyHash;

    // The namespace that this bucket is used for.
    NamespaceString _ns;

    // The metadata of the data that this bucket contains.
    BucketMetadata _metadata;

    // Top-level hashed field names of the measurements that have been inserted into the bucket.
    StringSet _fieldNames;

    // Top-level hashed new field names that have not yet been committed into the bucket.
    StringSet _uncommittedFieldNames;

    // Time field for the measurements that have been inserted into the bucket.
    std::string _timeField;

    // Minimum timestamp over contained measurements
    Date_t _minTime;

    // The minimum and maximum values for each field in the bucket.
    timeseries::MinMax _minmax;

    // The reference schema for measurements in this bucket. May reflect schema of uncommitted
    // measurements.
    timeseries::Schema _schema;

    // The total size in bytes of the bucket's BSON serialization, including measurements to be
    // inserted.
    uint64_t _size = 0;

    // The total number of measurements in the bucket, including uncommitted measurements and
    // measurements to be inserted.
    uint32_t _numMeasurements = 0;

    // The number of committed measurements in the bucket.
    uint32_t _numCommittedMeasurements = 0;

    // Whether the bucket has been marked for a rollover action. It can be marked for closure due to
    // number of measurements, size, or schema changes, or it can be marked for archival due to time
    // range.
    RolloverAction _rolloverAction = RolloverAction::kNone;

    // Whether this bucket was kept open after exceeding the bucket max size to improve bucketing
    // performance for large measurements.
    bool _keptOpenDueToLargeMeasurements = false;

    // The batch that has been prepared and is currently in the process of being committed, if
    // any.
    std::shared_ptr<WriteBatch> _preparedBatch;

    // Batches, per operation, that haven't been committed or aborted yet.
    stdx::unordered_map<OperationId, std::shared_ptr<WriteBatch>> _batches;

    // If the bucket is in idleBuckets, then its position is recorded here.
    boost::optional<Stripe::IdleList::iterator> _idleListEntry = boost::none;

    // Approximate memory usage of this bucket.
    uint64_t _memoryUsage = sizeof(*this);
};

/**
 * Bundle of information that 'insert' needs to pass down to helper methods that may create a new
 * bucket.
 */
struct BucketCatalog::CreationInfo {
    const BucketKey& key;
    StripeNumber stripe;
    const Date_t& time;
    const TimeseriesOptions& options;
    ExecutionStatsController& stats;
    ClosedBuckets* closedBuckets;
    bool openedDuetoMetadata = true;
};

BucketCatalog::WriteBatch::WriteBatch(const BucketHandle& bucket,
                                      OperationId opId,
                                      ExecutionStatsController& stats)
    : _bucket{bucket}, _opId(opId), _stats(stats) {}

bool BucketCatalog::WriteBatch::claimCommitRights() {
    return !_commitRights.swap(true);
}

StatusWith<BucketCatalog::CommitInfo> BucketCatalog::WriteBatch::getResult() {
    if (!_promise.getFuture().isReady()) {
        _stats.incNumWaits();
    }
    return _promise.getFuture().getNoThrow();
}

const BucketCatalog::BucketHandle& BucketCatalog::WriteBatch::bucket() const {
    return _bucket;
}

const std::vector<BSONObj>& BucketCatalog::WriteBatch::measurements() const {
    return _measurements;
}

const BSONObj& BucketCatalog::WriteBatch::min() const {
    return _min;
}

const BSONObj& BucketCatalog::WriteBatch::max() const {
    return _max;
}

const StringMap<std::size_t>& BucketCatalog::WriteBatch::newFieldNamesToBeInserted() const {
    return _newFieldNamesToBeInserted;
}

uint32_t BucketCatalog::WriteBatch::numPreviouslyCommittedMeasurements() const {
    return _numPreviouslyCommittedMeasurements;
}

bool BucketCatalog::WriteBatch::finished() const {
    return _promise.getFuture().isReady();
}

BSONObj BucketCatalog::WriteBatch::toBSON() const {
    auto toFieldName = [](const auto& nameHashPair) { return nameHashPair.first; };
    return BSON("docs" << _measurements << "bucketMin" << _min << "bucketMax" << _max
                       << "numCommittedMeasurements" << int(_numPreviouslyCommittedMeasurements)
                       << "newFieldNamesToBeInserted"
                       << std::set<std::string>(
                              boost::make_transform_iterator(_newFieldNamesToBeInserted.begin(),
                                                             toFieldName),
                              boost::make_transform_iterator(_newFieldNamesToBeInserted.end(),
                                                             toFieldName)));
}

void BucketCatalog::WriteBatch::_addMeasurement(const BSONObj& doc) {
    _measurements.push_back(doc);
}

void BucketCatalog::WriteBatch::_recordNewFields(Bucket* bucket, NewFieldNames&& fields) {
    for (auto&& field : fields) {
        _newFieldNamesToBeInserted[field] = field.hash();
        bucket->_uncommittedFieldNames.emplace(field);
    }
}

void BucketCatalog::WriteBatch::_prepareCommit(Bucket* bucket) {
    invariant(_commitRights.load());
    _numPreviouslyCommittedMeasurements = bucket->_numCommittedMeasurements;

    // Filter out field names that were new at the time of insertion, but have since been committed
    // by someone else.
    for (auto it = _newFieldNamesToBeInserted.begin(); it != _newFieldNamesToBeInserted.end();) {
        StringMapHashedKey fieldName(it->first, it->second);
        bucket->_uncommittedFieldNames.erase(fieldName);
        if (bucket->_fieldNames.contains(fieldName)) {
            _newFieldNamesToBeInserted.erase(it++);
            continue;
        }

        bucket->_fieldNames.emplace(fieldName);
        ++it;
    }

    for (const auto& doc : _measurements) {
        bucket->_minmax.update(
            doc, bucket->_metadata.getMetaField(), bucket->_metadata.getComparator());
    }

    const bool isUpdate = _numPreviouslyCommittedMeasurements > 0;
    if (isUpdate) {
        _min = bucket->_minmax.minUpdates();
        _max = bucket->_minmax.maxUpdates();
    } else {
        _min = bucket->_minmax.min();
        _max = bucket->_minmax.max();

        // Approximate minmax memory usage by taking sizes of initial commit. Subsequent updates may
        // add fields but are most likely just to update values.
        bucket->_memoryUsage += _min.objsize();
        bucket->_memoryUsage += _max.objsize();
    }
}

void BucketCatalog::WriteBatch::_finish(const CommitInfo& info) {
    invariant(_commitRights.load());
    _promise.emplaceValue(info);
}

void BucketCatalog::WriteBatch::_abort(const Status& status) {
    if (finished()) {
        return;
    }

    _promise.setError(status);
}

BucketCatalog& BucketCatalog::get(ServiceContext* svcCtx) {
    return getBucketCatalog(svcCtx);
}

BucketCatalog& BucketCatalog::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

Status BucketCatalog::reopenBucket(OperationContext* opCtx,
                                   const CollectionPtr& coll,
                                   const BSONObj& bucketDoc) {
    const NamespaceString ns = coll->ns().getTimeseriesViewNamespace();
    const boost::optional<TimeseriesOptions> options = coll->getTimeseriesOptions();
    invariant(options,
              str::stream() << "Attempting to reopen a bucket for a non-timeseries collection: "
                            << ns);

    BSONElement bucketIdElem = bucketDoc.getField(timeseries::kBucketIdFieldName);
    if (bucketIdElem.eoo() || bucketIdElem.type() != BSONType::jstOID) {
        return {ErrorCodes::BadValue,
                str::stream() << timeseries::kBucketIdFieldName
                              << " is missing or not an ObjectId"};
    }

    // Validate the bucket document against the schema.
    auto result = coll->checkValidation(opCtx, bucketDoc);
    if (result.first != Collection::SchemaValidationResult::kPass) {
        return result.second;
    }

    BSONElement metadata;
    auto metaFieldName = options->getMetaField();
    if (metaFieldName) {
        metadata = bucketDoc.getField(*metaFieldName);
    }

    // Buckets are spread across independently-lockable stripes to improve parallelism. We map a
    // bucket to a stripe by hashing the BucketKey.
    auto key = BucketKey{ns, BucketMetadata{metadata, coll->getDefaultCollator()}};
    auto stripeNumber = _getStripeNumber(key);

    auto bucketId = bucketIdElem.OID();
    std::unique_ptr<Bucket> bucket = std::make_unique<Bucket>(bucketId, stripeNumber, key.hash);

    // Initialize the remaining member variables from the bucket document.
    bucket->_ns = ns;
    bucket->_metadata = key.metadata;
    bucket->_timeField = options->getTimeField().toString();
    bucket->_size = bucketDoc.objsize();
    bucket->_minTime = bucketDoc.getObjectField(timeseries::kBucketControlFieldName)
                           .getObjectField(timeseries::kBucketControlMinFieldName)
                           .getField(options->getTimeField())
                           .Date();

    // Populate the top-level data field names.
    const BSONObj& dataObj = bucketDoc.getObjectField(timeseries::kBucketDataFieldName);
    for (const BSONElement& dataElem : dataObj) {
        auto hashedKey = StringSet::hasher().hashed_key(dataElem.fieldName());
        bucket->_fieldNames.emplace(hashedKey);
    }

    auto swMinMax = timeseries::generateMinMaxFromBucketDoc(bucketDoc, coll->getDefaultCollator());
    if (!swMinMax.isOK()) {
        return swMinMax.getStatus();
    }
    bucket->_minmax = std::move(swMinMax.getValue());

    auto swSchema = timeseries::generateSchemaFromBucketDoc(bucketDoc, coll->getDefaultCollator());
    if (!swSchema.isOK()) {
        return swSchema.getStatus();
    }
    bucket->_schema = std::move(swSchema.getValue());

    uint32_t numMeasurements = 0;
    const bool isCompressed = timeseries::isCompressedBucket(bucketDoc);
    const BSONElement timeColumnElem = dataObj.getField(options->getTimeField());

    if (isCompressed && timeColumnElem.type() == BSONType::BinData) {
        BSONColumn storage{timeColumnElem};
        numMeasurements = storage.size();
    } else {
        numMeasurements = timeColumnElem.Obj().nFields();
    }

    bucket->_numMeasurements = numMeasurements;
    bucket->_numCommittedMeasurements = numMeasurements;

    ExecutionStatsController stats = _getExecutionStats(ns);
    stats.incNumBucketsReopened();

    // Register the reopened bucket with the catalog.
    auto& stripe = _stripes[stripeNumber];
    stdx::lock_guard stripeLock{stripe.mutex};

    ClosedBuckets closedBuckets;
    _expireIdleBuckets(&stripe, stripeLock, stats, &closedBuckets);

    auto [it, inserted] = stripe.allBuckets.try_emplace(bucketId, std::move(bucket));
    tassert(6668200, "Expected bucket to be inserted", inserted);
    Bucket* unownedBucket = it->second.get();
    stripe.openBuckets[key] = unownedBucket;
    _initializeBucketState(bucketId);

    return Status::OK();
}

BSONObj BucketCatalog::getMetadata(const BucketHandle& handle) const {
    auto const& stripe = _stripes[handle.stripe];
    stdx::lock_guard stripeLock{stripe.mutex};

    const Bucket* bucket = _findBucket(stripe, stripeLock, handle.id);
    if (!bucket) {
        return {};
    }

    return bucket->_metadata.toBSON();
}

StatusWith<BucketCatalog::InsertResult> BucketCatalog::insert(
    OperationContext* opCtx,
    const NamespaceString& ns,
    const StringData::ComparatorInterface* comparator,
    const TimeseriesOptions& options,
    const BSONObj& doc,
    CombineWithInsertsFromOtherClients combine) {

    auto timeElem = doc[options.getTimeField()];
    if (!timeElem || BSONType::Date != timeElem.type()) {
        return {ErrorCodes::BadValue,
                str::stream() << "'" << options.getTimeField() << "' must be present and contain a "
                              << "valid BSON UTC datetime value"};
    }
    auto time = timeElem.Date();

    ExecutionStatsController stats = _getExecutionStats(ns);

    BSONElement metadata;
    auto metaFieldName = options.getMetaField();
    if (metaFieldName) {
        metadata = doc[*metaFieldName];
    }

    // Buckets are spread across independently-lockable stripes to improve parallelism. We map a
    // bucket to a stripe by hashing the BucketKey.
    auto key = BucketKey{ns, BucketMetadata{metadata, comparator}};
    auto stripeNumber = _getStripeNumber(key);

    ClosedBuckets closedBuckets;
    CreationInfo info{key, stripeNumber, time, options, stats, &closedBuckets};

    auto& stripe = _stripes[stripeNumber];
    stdx::lock_guard stripeLock{stripe.mutex};

    Bucket* bucket = _useOrCreateBucket(&stripe, stripeLock, info);
    invariant(bucket);

    NewFieldNames newFieldNamesToBeInserted;
    uint32_t sizeToBeAdded = 0;
    bucket->_calculateBucketFieldsAndSizeChange(
        doc, options.getMetaField(), &newFieldNamesToBeInserted, &sizeToBeAdded);

    auto determineRolloverAction = [&](Bucket* bucket) -> RolloverAction {
        const bool canArchive = feature_flags::gTimeseriesScalabilityImprovements.isEnabled(
            serverGlobalParams.featureCompatibility);

        if (bucket->schemaIncompatible(doc, metaFieldName, comparator)) {
            stats.incNumBucketsClosedDueToSchemaChange();
            return RolloverAction::kClose;
        }
        if (bucket->_numMeasurements == static_cast<std::uint64_t>(gTimeseriesBucketMaxCount)) {
            stats.incNumBucketsClosedDueToCount();
            return RolloverAction::kClose;
        }
        auto bucketTime = bucket->getTime();
        if (time - bucketTime >= Seconds(*options.getBucketMaxSpanSeconds())) {
            if (canArchive) {
                stats.incNumBucketsArchivedDueToTimeForward();
                return RolloverAction::kArchive;
            } else {
                stats.incNumBucketsClosedDueToTimeForward();
                return RolloverAction::kClose;
            }
        }
        if (time < bucketTime) {
            if (canArchive) {
                stats.incNumBucketsArchivedDueToTimeBackward();
                return RolloverAction::kArchive;
            } else {
                stats.incNumBucketsClosedDueToTimeBackward();
                return RolloverAction::kClose;
            }
        }
        if (bucket->_size + sizeToBeAdded > static_cast<std::uint64_t>(gTimeseriesBucketMaxSize)) {
            bool keepBucketOpenForLargeMeasurements =
                bucket->_numMeasurements < static_cast<std::uint64_t>(gTimeseriesBucketMinCount) &&
                feature_flags::gTimeseriesScalabilityImprovements.isEnabled(
                    serverGlobalParams.featureCompatibility);
            if (keepBucketOpenForLargeMeasurements) {
                // Instead of packing the bucket to the BSON size limit, 16MB, we'll limit the max
                // bucket size to 12MB. This is to leave some space in the bucket if we need to add
                // new internal fields to existing, full buckets.
                static constexpr size_t largeMeasurementsMaxBucketSize =
                    BSONObjMaxUserSize - (4 * 1024 * 1024);

                if (bucket->_size + sizeToBeAdded > largeMeasurementsMaxBucketSize) {
                    stats.incNumBucketsClosedDueToSize();
                    return RolloverAction::kClose;
                }

                // There's enough space to add this measurement and we're still below the large
                // measurement threshold.
                if (!bucket->_keptOpenDueToLargeMeasurements) {
                    // Only increment this metric once per bucket.
                    bucket->_keptOpenDueToLargeMeasurements = true;
                    stats.incNumBucketsKeptOpenDueToLargeMeasurements();
                }
                return RolloverAction::kNone;
            } else {
                stats.incNumBucketsClosedDueToSize();
                return RolloverAction::kClose;
            }
        }
        return RolloverAction::kNone;
    };

    if (!bucket->_ns.isEmpty()) {
        auto action = determineRolloverAction(bucket);
        if (action != RolloverAction::kNone) {
            info.openedDuetoMetadata = false;
            bucket = _rollover(&stripe, stripeLock, bucket, info, action);

            bucket->_calculateBucketFieldsAndSizeChange(
                doc, options.getMetaField(), &newFieldNamesToBeInserted, &sizeToBeAdded);
        }
    }

    auto batch = bucket->_activeBatch(getOpId(opCtx, combine), stats);
    batch->_addMeasurement(doc);
    batch->_recordNewFields(bucket, std::move(newFieldNamesToBeInserted));

    bucket->_numMeasurements++;
    bucket->_size += sizeToBeAdded;
    if (bucket->_ns.isEmpty()) {
        // The namespace and metadata only need to be set if this bucket was newly created.
        bucket->_ns = ns;
        bucket->_metadata = key.metadata;

        // The namespace is stored two times: the bucket itself and openBuckets.
        // We don't have a great approximation for the
        // _schema size, so we use initial document size minus metadata as an approximation. Since
        // the metadata itself is stored once, in the bucket, we can combine the two and just use
        // the initial document size. A unique pointer to the bucket is stored once: allBuckets. A
        // raw pointer to the bucket is stored at most twice: openBuckets, idleBuckets.
        bucket->_memoryUsage += (ns.size() * 2) + doc.objsize() + sizeof(Bucket) +
            sizeof(std::unique_ptr<Bucket>) + (sizeof(Bucket*) * 2);

        bucket->_schema.update(doc, options.getMetaField(), comparator);
    } else {
        _memoryUsage.fetchAndSubtract(bucket->_memoryUsage);
    }
    _memoryUsage.fetchAndAdd(bucket->_memoryUsage);

    return InsertResult{batch, closedBuckets};
}

Status BucketCatalog::prepareCommit(std::shared_ptr<WriteBatch> batch) {
    auto getBatchStatus = [&] { return batch->_promise.getFuture().getNoThrow().getStatus(); };

    if (batch->finished()) {
        // In this case, someone else aborted the batch behind our back. Oops.
        return getBatchStatus();
    }

    auto& stripe = _stripes[batch->bucket().stripe];
    _waitToCommitBatch(&stripe, batch);

    stdx::lock_guard stripeLock{stripe.mutex};
    Bucket* bucket =
        _useBucketInState(&stripe, stripeLock, batch->bucket().id, BucketState::kPrepared);

    if (batch->finished()) {
        // Someone may have aborted it while we were waiting.
        return getBatchStatus();
    } else if (!bucket) {
        _abort(&stripe, stripeLock, batch, getTimeseriesBucketClearedError(batch->bucket().id));
        return getBatchStatus();
    }

    auto prevMemoryUsage = bucket->_memoryUsage;
    batch->_prepareCommit(bucket);
    _memoryUsage.fetchAndAdd(bucket->_memoryUsage - prevMemoryUsage);

    return Status::OK();
}

boost::optional<BucketCatalog::ClosedBucket> BucketCatalog::finish(
    std::shared_ptr<WriteBatch> batch, const CommitInfo& info) {
    invariant(!batch->finished());

    boost::optional<ClosedBucket> closedBucket;

    batch->_finish(info);

    auto& stripe = _stripes[batch->bucket().stripe];
    stdx::lock_guard stripeLock{stripe.mutex};

    Bucket* bucket =
        _useBucketInState(&stripe, stripeLock, batch->bucket().id, BucketState::kNormal);
    if (bucket) {
        bucket->_preparedBatch.reset();
    }

    auto& stats = batch->_stats;
    stats.incNumCommits();
    if (batch->numPreviouslyCommittedMeasurements() == 0) {
        stats.incNumBucketInserts();
    } else {
        stats.incNumBucketUpdates();
    }

    stats.incNumMeasurementsCommitted(batch->measurements().size());
    if (bucket) {
        bucket->_numCommittedMeasurements += batch->measurements().size();
    }

    if (!bucket) {
        // It's possible that we cleared the bucket in between preparing the commit and finishing
        // here. In this case, we should abort any other ongoing batches and clear the bucket from
        // the catalog so it's not hanging around idle.
        auto it = stripe.allBuckets.find(batch->bucket().id);
        if (it != stripe.allBuckets.end()) {
            bucket = it->second.get();
            bucket->_preparedBatch.reset();
            _abort(&stripe,
                   stripeLock,
                   bucket,
                   nullptr,
                   getTimeseriesBucketClearedError(bucket->id(), bucket->_ns));
        }
    } else if (bucket->allCommitted()) {
        switch (bucket->_rolloverAction) {
            case RolloverAction::kClose: {
                closedBucket = ClosedBucket{
                    bucket->id(), bucket->getTimeField().toString(), bucket->numMeasurements()};
                _removeBucket(&stripe, stripeLock, bucket, false);
                break;
            }
            case RolloverAction::kArchive: {
                _archiveBucket(&stripe, stripeLock, bucket);
                break;
            }
            case RolloverAction::kNone: {
                _markBucketIdle(&stripe, stripeLock, bucket);
                break;
            }
        }
    }
    return closedBucket;
}

void BucketCatalog::abort(std::shared_ptr<WriteBatch> batch, const Status& status) {
    invariant(batch);
    invariant(batch->_commitRights.load());

    if (batch->finished()) {
        return;
    }

    auto& stripe = _stripes[batch->bucket().stripe];
    stdx::lock_guard stripeLock{stripe.mutex};

    _abort(&stripe, stripeLock, batch, status);
}

void BucketCatalog::clear(const OID& oid) {
    auto result = _setBucketState(oid, BucketState::kCleared);
    if (result && *result == BucketState::kPreparedAndCleared) {
        hangTimeseriesDirectModificationBeforeWriteConflict.pauseWhileSet();
        throwWriteConflictException();
    }
}

void BucketCatalog::clear(const std::function<bool(const NamespaceString&)>& shouldClear) {
    for (auto& stripe : _stripes) {
        stdx::lock_guard stripeLock{stripe.mutex};
        for (auto it = stripe.allBuckets.begin(); it != stripe.allBuckets.end();) {
            auto nextIt = std::next(it);

            const auto& bucket = it->second;
            if (shouldClear(bucket->_ns)) {
                {
                    stdx::lock_guard catalogLock{_mutex};
                    _executionStats.erase(bucket->_ns);
                }
                _abort(&stripe,
                       stripeLock,
                       bucket.get(),
                       nullptr,
                       getTimeseriesBucketClearedError(bucket->id(), bucket->_ns));
            }

            it = nextIt;
        }
    }
}

void BucketCatalog::clear(const NamespaceString& ns) {
    clear([&ns](const NamespaceString& bucketNs) { return bucketNs == ns; });
}

void BucketCatalog::clear(StringData dbName) {
    clear([&dbName](const NamespaceString& bucketNs) { return bucketNs.db() == dbName; });
}

void BucketCatalog::_appendExecutionStatsToBuilder(const ExecutionStats* stats,
                                                   BSONObjBuilder* builder) const {
    builder->appendNumber("numBucketInserts", stats->numBucketInserts.load());
    builder->appendNumber("numBucketUpdates", stats->numBucketUpdates.load());
    builder->appendNumber("numBucketsOpenedDueToMetadata",
                          stats->numBucketsOpenedDueToMetadata.load());
    builder->appendNumber("numBucketsClosedDueToCount", stats->numBucketsClosedDueToCount.load());
    builder->appendNumber("numBucketsClosedDueToSchemaChange",
                          stats->numBucketsClosedDueToSchemaChange.load());
    builder->appendNumber("numBucketsClosedDueToSize", stats->numBucketsClosedDueToSize.load());
    builder->appendNumber("numBucketsClosedDueToTimeForward",
                          stats->numBucketsClosedDueToTimeForward.load());
    builder->appendNumber("numBucketsClosedDueToTimeBackward",
                          stats->numBucketsClosedDueToTimeBackward.load());
    builder->appendNumber("numBucketsClosedDueToMemoryThreshold",
                          stats->numBucketsClosedDueToMemoryThreshold.load());

    auto commits = stats->numCommits.load();
    builder->appendNumber("numCommits", commits);
    builder->appendNumber("numWaits", stats->numWaits.load());
    auto measurementsCommitted = stats->numMeasurementsCommitted.load();
    builder->appendNumber("numMeasurementsCommitted", measurementsCommitted);
    if (commits) {
        builder->appendNumber("avgNumMeasurementsPerCommit", measurementsCommitted / commits);
    }

    if (feature_flags::gTimeseriesScalabilityImprovements.isEnabled(
            serverGlobalParams.featureCompatibility)) {
        builder->appendNumber("numBucketsArchivedDueToTimeForward",
                              stats->numBucketsArchivedDueToTimeForward.load());
        builder->appendNumber("numBucketsArchivedDueToTimeBackward",
                              stats->numBucketsArchivedDueToTimeBackward.load());
        builder->appendNumber("numBucketsArchivedDueToMemoryThreshold",
                              stats->numBucketsArchivedDueToMemoryThreshold.load());
        builder->appendNumber("numBucketsReopened", stats->numBucketsReopened.load());
        builder->appendNumber("numBucketsKeptOpenDueToLargeMeasurements",
                              stats->numBucketsKeptOpenDueToLargeMeasurements.load());
    }
}

void BucketCatalog::appendExecutionStats(const NamespaceString& ns, BSONObjBuilder* builder) const {
    const std::shared_ptr<ExecutionStats> stats = _getExecutionStats(ns);
    _appendExecutionStatsToBuilder(stats.get(), builder);
}

void BucketCatalog::appendGlobalExecutionStats(BSONObjBuilder* builder) const {
    _appendExecutionStatsToBuilder(&_globalExecutionStats, builder);
}

BucketCatalog::BucketMetadata::BucketMetadata(BSONElement elem,
                                              const StringData::ComparatorInterface* comparator)
    : _metadataElement(elem), _comparator(comparator) {
    if (_metadataElement) {
        BSONObjBuilder objBuilder;
        // We will get an object of equal size, just with reordered fields.
        objBuilder.bb().reserveBytes(_metadataElement.size());
        normalizeTopLevel(&objBuilder, _metadataElement);
        _metadata = objBuilder.obj();
    }
    // Updates the BSONElement to refer to the copied BSONObj.
    _metadataElement = _metadata.firstElement();
}

bool BucketCatalog::BucketMetadata::operator==(const BucketMetadata& other) const {
    return _metadataElement.binaryEqualValues(other._metadataElement);
}

const BSONObj& BucketCatalog::BucketMetadata::toBSON() const {
    return _metadata;
}

StringData BucketCatalog::BucketMetadata::getMetaField() const {
    return StringData(_metadataElement.fieldName());
}

const StringData::ComparatorInterface* BucketCatalog::BucketMetadata::getComparator() const {
    return _comparator;
}

BucketCatalog::BucketKey::BucketKey(const NamespaceString& n, const BucketMetadata& m)
    : ns(n), metadata(m), hash(absl::Hash<BucketKey>{}(*this)) {}

std::size_t BucketCatalog::BucketHasher::operator()(const BucketKey& key) const {
    // Use the default absl hasher.
    return key.hash;
}

std::size_t BucketCatalog::PreHashed::operator()(const BucketKey::Hash& key) const {
    return key;
}

BucketCatalog::StripeNumber BucketCatalog::_getStripeNumber(const BucketKey& key) {
    return key.hash % kNumberOfStripes;
}

const BucketCatalog::Bucket* BucketCatalog::_findBucket(const Stripe& stripe,
                                                        WithLock,
                                                        const OID& id,
                                                        ReturnClearedBuckets mode) const {
    auto it = stripe.allBuckets.find(id);
    if (it != stripe.allBuckets.end()) {
        if (mode == ReturnClearedBuckets::kYes) {
            return it->second.get();
        }

        auto state = _getBucketState(id);
        if (state && state != BucketState::kCleared && state != BucketState::kPreparedAndCleared) {
            return it->second.get();
        }
    }
    return nullptr;
}

BucketCatalog::Bucket* BucketCatalog::_useBucket(Stripe* stripe,
                                                 WithLock stripeLock,
                                                 const OID& id,
                                                 ReturnClearedBuckets mode) {
    return const_cast<Bucket*>(_findBucket(*stripe, stripeLock, id, mode));
}

BucketCatalog::Bucket* BucketCatalog::_useBucketInState(Stripe* stripe,
                                                        WithLock,
                                                        const OID& id,
                                                        BucketState targetState) {
    auto it = stripe->allBuckets.find(id);
    if (it != stripe->allBuckets.end()) {
        auto state = _setBucketState(it->second->_id, targetState);
        if (state && state != BucketState::kCleared && state != BucketState::kPreparedAndCleared) {
            return it->second.get();
        }
    }
    return nullptr;
}

BucketCatalog::Bucket* BucketCatalog::_useOrCreateBucket(Stripe* stripe,
                                                         WithLock stripeLock,
                                                         const CreationInfo& info) {
    auto it = stripe->openBuckets.find(info.key);
    if (it == stripe->openBuckets.end()) {
        // No open bucket for this metadata.
        return _allocateBucket(stripe, stripeLock, info);
    }

    Bucket* bucket = it->second;

    auto state = _getBucketState(bucket->id());
    if (state == BucketState::kNormal || state == BucketState::kPrepared) {
        _markBucketNotIdle(stripe, stripeLock, bucket);
        return bucket;
    }

    _abort(stripe,
           stripeLock,
           bucket,
           nullptr,
           getTimeseriesBucketClearedError(bucket->id(), bucket->_ns));

    return _allocateBucket(stripe, stripeLock, info);
}

void BucketCatalog::_waitToCommitBatch(Stripe* stripe, const std::shared_ptr<WriteBatch>& batch) {
    while (true) {
        std::shared_ptr<WriteBatch> current;

        {
            stdx::lock_guard stripeLock{stripe->mutex};
            Bucket* bucket =
                _useBucket(stripe, stripeLock, batch->bucket().id, ReturnClearedBuckets::kNo);
            if (!bucket || batch->finished()) {
                return;
            }

            current = bucket->_preparedBatch;
            if (!current) {
                // No other batches for this bucket are currently committing, so we can proceed.
                bucket->_preparedBatch = batch;
                bucket->_batches.erase(batch->_opId);
                return;
            }
        }

        // We have to wait for someone else to finish.
        current->getResult().getStatus().ignore();  // We don't care about the result.
    }
}

void BucketCatalog::_removeBucket(Stripe* stripe,
                                  WithLock stripeLock,
                                  Bucket* bucket,
                                  bool archiving) {
    invariant(bucket->_batches.empty());
    invariant(!bucket->_preparedBatch);

    auto allIt = stripe->allBuckets.find(bucket->id());
    invariant(allIt != stripe->allBuckets.end());

    _memoryUsage.fetchAndSubtract(bucket->_memoryUsage);
    _markBucketNotIdle(stripe, stripeLock, bucket);

    // If the bucket was rolled over, then there may be a different open bucket for this metadata.
    auto openIt = stripe->openBuckets.find({bucket->_ns, bucket->_metadata});
    if (openIt != stripe->openBuckets.end() && openIt->second == bucket) {
        stripe->openBuckets.erase(openIt);
    }

    // If we are cleaning up while archiving a bucket, then we want to preserve its state. Otherwise
    // we can remove the state from the catalog altogether.
    if (!archiving) {
        _eraseBucketState(bucket->id());
    }

    stripe->allBuckets.erase(allIt);
}

void BucketCatalog::_archiveBucket(Stripe* stripe, WithLock stripeLock, Bucket* bucket) {
    bool archived = false;
    auto& archivedSet = stripe->archivedBuckets[bucket->keyHash()];
    auto it = archivedSet.find(bucket->getTime());
    if (it == archivedSet.end()) {
        archivedSet.emplace(bucket->getTime(),
                            ArchivedBucket{bucket->id(),
                                           bucket->getTimeField().toString(),
                                           bucket->numMeasurements()});

        long long memory = _marginalMemoryUsageForArchivedBucket(archivedSet[bucket->getTime()],
                                                                 archivedSet.size() == 1);
        _memoryUsage.fetchAndAdd(memory);

        archived = true;
    }
    _removeBucket(stripe, stripeLock, bucket, archived);
}

void BucketCatalog::_abort(Stripe* stripe,
                           WithLock stripeLock,
                           std::shared_ptr<WriteBatch> batch,
                           const Status& status) {
    // Before we access the bucket, make sure it's still there.
    Bucket* bucket = _useBucket(stripe, stripeLock, batch->bucket().id, ReturnClearedBuckets::kYes);
    if (!bucket) {
        // Special case, bucket has already been cleared, and we need only abort this batch.
        batch->_abort(status);
        return;
    }

    // Proceed to abort any unprepared batches and remove the bucket if possible
    _abort(stripe, stripeLock, bucket, batch, status);
}

void BucketCatalog::_abort(Stripe* stripe,
                           WithLock stripeLock,
                           Bucket* bucket,
                           std::shared_ptr<WriteBatch> batch,
                           const Status& status) {
    // Abort any unprepared batches. This should be safe since we have a lock on the stripe,
    // preventing anyone else from using these.
    for (const auto& [_, current] : bucket->_batches) {
        current->_abort(status);
    }
    bucket->_batches.clear();

    bool doRemove = true;  // We shouldn't remove the bucket if there's a prepared batch outstanding
                           // and it's not the one we manage. In that case, we don't know what the
                           // user is doing with it, but we need to keep the bucket around until
                           // that batch is finished.
    if (auto& prepared = bucket->_preparedBatch) {
        if (prepared == batch) {
            // We own the prepared batch, so we can go ahead and abort it and remove the bucket.
            prepared->_abort(status);
            prepared.reset();
        } else {
            doRemove = false;
        }
    }

    if (doRemove) {
        _removeBucket(stripe, stripeLock, bucket, false);
    }
}

void BucketCatalog::_markBucketIdle(Stripe* stripe, WithLock stripeLock, Bucket* bucket) {
    invariant(bucket);
    stripe->idleBuckets.push_front(bucket);
    bucket->_idleListEntry = stripe->idleBuckets.begin();
}

void BucketCatalog::_markBucketNotIdle(Stripe* stripe, WithLock stripeLock, Bucket* bucket) {
    invariant(bucket);
    if (bucket->_idleListEntry) {
        stripe->idleBuckets.erase(*bucket->_idleListEntry);
        bucket->_idleListEntry = boost::none;
    }
}

void BucketCatalog::_expireIdleBuckets(Stripe* stripe,
                                       WithLock stripeLock,
                                       ExecutionStatsController& stats,
                                       std::vector<BucketCatalog::ClosedBucket>* closedBuckets) {
    // As long as we still need space and have entries and remaining attempts, close idle buckets.
    int32_t numExpired = 0;

    const bool canArchive = feature_flags::gTimeseriesScalabilityImprovements.isEnabled(
        serverGlobalParams.featureCompatibility);

    while (!stripe->idleBuckets.empty() &&
           _memoryUsage.load() > getTimeseriesIdleBucketExpiryMemoryUsageThresholdBytes() &&
           numExpired <= gTimeseriesIdleBucketExpiryMaxCountPerAttempt) {
        Bucket* bucket = stripe->idleBuckets.back();

        if (canArchive) {
            _archiveBucket(stripe, stripeLock, bucket);
            stats.incNumBucketsArchivedDueToMemoryThreshold();
        } else {
            ClosedBucket closed{
                bucket->id(), bucket->getTimeField().toString(), bucket->numMeasurements()};
            _removeBucket(stripe, stripeLock, bucket, false);
            stats.incNumBucketsClosedDueToMemoryThreshold();
            closedBuckets->push_back(closed);
        }

        ++numExpired;
    }

    while (canArchive && !stripe->archivedBuckets.empty() &&
           _memoryUsage.load() > getTimeseriesIdleBucketExpiryMemoryUsageThresholdBytes() &&
           numExpired <= gTimeseriesIdleBucketExpiryMaxCountPerAttempt) {

        auto& [hash, archivedSet] = *stripe->archivedBuckets.begin();
        invariant(!archivedSet.empty());

        auto& [timestamp, bucket] = *archivedSet.begin();
        ClosedBucket closed{bucket.bucketId, bucket.timeField, bucket.numMeasurements, true};

        long long memory = _marginalMemoryUsageForArchivedBucket(bucket, archivedSet.size() == 1);
        _eraseBucketState(bucket.bucketId);
        if (archivedSet.size() == 1) {
            // If this is the only entry, erase the whole map so we don't leave it empty.
            stripe->archivedBuckets.erase(stripe->archivedBuckets.begin());
        } else {
            // Otherwise just erase this bucket from the map.
            archivedSet.erase(archivedSet.begin());
        }
        _memoryUsage.fetchAndSubtract(memory);

        stats.incNumBucketsClosedDueToMemoryThreshold();
        closedBuckets->push_back(closed);
        ++numExpired;
    }
}

BucketCatalog::Bucket* BucketCatalog::_allocateBucket(Stripe* stripe,
                                                      WithLock stripeLock,
                                                      const CreationInfo& info) {
    _expireIdleBuckets(stripe, stripeLock, info.stats, info.closedBuckets);

    auto [bucketId, roundedTime] = generateBucketId(info.time, info.options);

    auto [it, inserted] = stripe->allBuckets.try_emplace(
        bucketId, std::make_unique<Bucket>(bucketId, info.stripe, info.key.hash));
    tassert(6130900, "Expected bucket to be inserted", inserted);
    Bucket* bucket = it->second.get();
    stripe->openBuckets[info.key] = bucket;
    _initializeBucketState(bucketId);

    if (info.openedDuetoMetadata) {
        info.stats.incNumBucketsOpenedDueToMetadata();
    }

    bucket->_timeField = info.options.getTimeField().toString();
    bucket->_minTime = roundedTime;

    // Make sure we set the control.min time field to match the rounded _id timestamp.
    auto controlDoc = buildControlMinTimestampDoc(info.options.getTimeField(), roundedTime);
    bucket->_minmax.update(
        controlDoc, bucket->_metadata.getMetaField(), bucket->_metadata.getComparator());

    return bucket;
}

BucketCatalog::Bucket* BucketCatalog::_rollover(Stripe* stripe,
                                                WithLock stripeLock,
                                                Bucket* bucket,
                                                const CreationInfo& info,
                                                RolloverAction action) {
    invariant(action != RolloverAction::kNone);
    if (bucket->allCommitted()) {
        // The bucket does not contain any measurements that are yet to be committed, so we can take
        // action now.
        if (action == RolloverAction::kClose) {
            info.closedBuckets->push_back(ClosedBucket{
                bucket->id(), bucket->getTimeField().toString(), bucket->numMeasurements()});

            _removeBucket(stripe, stripeLock, bucket, false);
        } else {
            invariant(action == RolloverAction::kArchive);
            _archiveBucket(stripe, stripeLock, bucket);
        }
    } else {
        // We must keep the bucket around until all measurements are committed committed, just mark
        // the action we chose now so it we know what to do when the last batch finishes.
        bucket->_rolloverAction = action;
    }

    return _allocateBucket(stripe, stripeLock, info);
}

BucketCatalog::ExecutionStatsController BucketCatalog::_getExecutionStats(
    const NamespaceString& ns) {
    stdx::lock_guard catalogLock{_mutex};
    auto it = _executionStats.find(ns);
    if (it != _executionStats.end()) {
        return {it->second, &_globalExecutionStats};
    }

    auto res = _executionStats.emplace(ns, std::make_shared<ExecutionStats>());
    return {res.first->second, &_globalExecutionStats};
}

std::shared_ptr<BucketCatalog::ExecutionStats> BucketCatalog::_getExecutionStats(
    const NamespaceString& ns) const {
    static const auto kEmptyStats{std::make_shared<ExecutionStats>()};

    stdx::lock_guard catalogLock{_mutex};

    auto it = _executionStats.find(ns);
    if (it != _executionStats.end()) {
        return it->second;
    }
    return kEmptyStats;
}

void BucketCatalog::_initializeBucketState(const OID& id) {
    stdx::lock_guard catalogLock{_mutex};
    _bucketStates.emplace(id, BucketState::kNormal);
}

void BucketCatalog::_eraseBucketState(const OID& id) {
    stdx::lock_guard catalogLock{_mutex};
    _bucketStates.erase(id);
}

boost::optional<BucketCatalog::BucketState> BucketCatalog::_getBucketState(const OID& id) const {
    stdx::lock_guard catalogLock{_mutex};
    auto it = _bucketStates.find(id);
    return it != _bucketStates.end() ? boost::make_optional(it->second) : boost::none;
}

boost::optional<BucketCatalog::BucketState> BucketCatalog::_setBucketState(const OID& id,
                                                                           BucketState target) {
    stdx::lock_guard catalogLock{_mutex};
    auto it = _bucketStates.find(id);
    if (it == _bucketStates.end()) {
        return boost::none;
    }

    auto& [_, state] = *it;
    switch (target) {
        case BucketState::kNormal: {
            if (state == BucketState::kPrepared) {
                state = BucketState::kNormal;
            } else if (state == BucketState::kPreparedAndCleared) {
                state = BucketState::kCleared;
            }
            break;
        }
        case BucketState::kPrepared: {
            if (state == BucketState::kNormal) {
                state = BucketState::kPrepared;
            }
            break;
        }
        case BucketState::kCleared: {
            if (state == BucketState::kNormal) {
                state = BucketState::kCleared;
            } else if (state == BucketState::kPrepared) {
                state = BucketState::kPreparedAndCleared;
            }
            break;
        }
        case BucketState::kPreparedAndCleared: {
            invariant(target != BucketState::kPreparedAndCleared);
        }
    }

    return state;
}

long long BucketCatalog::_marginalMemoryUsageForArchivedBucket(const ArchivedBucket& bucket,
                                                               bool onlyEntryForMatchingMetaHash) {
    return sizeof(std::size_t) + sizeof(Date_t) + sizeof(ArchivedBucket) + bucket.timeField.size() +
        (onlyEntryForMatchingMetaHash ? sizeof(decltype(Stripe::archivedBuckets)::value_type) : 0);
}

class BucketCatalog::ServerStatus : public ServerStatusSection {
    struct BucketCounts {
        BucketCounts& operator+=(const BucketCounts& other) {
            if (&other != this) {
                all += other.all;
                open += other.open;
                idle += other.idle;
            }
            return *this;
        }

        std::size_t all = 0;
        std::size_t open = 0;
        std::size_t idle = 0;
    };

    BucketCounts _getBucketCounts(const BucketCatalog& catalog) const {
        BucketCounts sum;
        for (auto const& stripe : catalog._stripes) {
            stdx::lock_guard stripeLock{stripe.mutex};
            sum += {stripe.allBuckets.size(), stripe.openBuckets.size(), stripe.idleBuckets.size()};
        }
        return sum;
    }

public:
    ServerStatus() : ServerStatusSection("bucketCatalog") {}

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx, const BSONElement&) const override {
        const auto& bucketCatalog = BucketCatalog::get(opCtx);
        {
            stdx::lock_guard catalogLock{bucketCatalog._mutex};
            if (bucketCatalog._executionStats.empty()) {
                return {};
            }
        }

        auto counts = _getBucketCounts(bucketCatalog);
        BSONObjBuilder builder;
        builder.appendNumber("numBuckets", static_cast<long long>(counts.all));
        builder.appendNumber("numOpenBuckets", static_cast<long long>(counts.open));
        builder.appendNumber("numIdleBuckets", static_cast<long long>(counts.idle));
        builder.appendNumber("memoryUsage",
                             static_cast<long long>(bucketCatalog._memoryUsage.load()));

        // Append the global execution stats for all namespaces.
        bucketCatalog.appendGlobalExecutionStats(&builder);

        return builder.obj();
    }
} bucketCatalogServerStatus;
}  // namespace mongo
