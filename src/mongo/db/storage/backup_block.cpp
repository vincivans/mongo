/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/storage/backup_block.h"

#include <boost/filesystem.hpp>
#include <set>

#include "mongo/base/string_data.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/storage/durable_catalog.h"
#include "mongo/db/storage/historical_ident_tracker.h"
#include "mongo/db/storage/storage_options.h"

namespace mongo {

namespace {

const std::set<std::string> kRequiredWTFiles = {
    "WiredTiger", "WiredTiger.backup", "WiredTigerHS.wt"};

const std::set<std::string> kRequiredMDBFiles = {"_mdb_catalog.wt", "sizeStorer.wt"};

}  // namespace

BackupBlock::BackupBlock(OperationContext* opCtx,
                         std::string filePath,
                         boost::optional<Timestamp> checkpointTimestamp,
                         std::uint64_t offset,
                         std::uint64_t length,
                         std::uint64_t fileSize)
    : _filePath(filePath), _offset(offset), _length(length), _fileSize(fileSize) {
    boost::filesystem::path path(filePath);
    _filenameStem = path.stem().string();
    _initialize(opCtx, checkpointTimestamp);
}

bool BackupBlock::isRequired() const {
    // Extract the filename from the path.
    boost::filesystem::path path(_filePath);
    const std::string filename = path.filename().string();

    // Check whether this is a required WiredTiger file.
    if (kRequiredWTFiles.find(filename) != kRequiredWTFiles.end()) {
        return true;
    }

    // Check if this is a journal file.
    if (StringData(filename).startsWith("WiredTigerLog.")) {
        return true;
    }

    // Check whether this is a required MongoDB file.
    if (kRequiredMDBFiles.find(filename) != kRequiredMDBFiles.end()) {
        return true;
    }

    // All files for the encrypted storage engine are required.
    boost::filesystem::path basePath(storageGlobalParams.dbpath);
    boost::filesystem::path keystoreBasePath(basePath / "key.store");
    if (StringData(path.string()).startsWith(keystoreBasePath.string())) {
        return true;
    }

    // Check if collection resides in an internal database (admin, local, or config).
    if (_nss.isOnInternalDb()) {
        return true;
    }

    // Check if collection is 'system.views'.
    if (_nss.isSystemDotViews()) {
        return true;
    }

    return false;
}

void BackupBlock::_setNamespaceString(const NamespaceString& nss) {
    // Remove "system.buckets." from time-series collection namespaces since it is an internal
    // detail that is not intended to be visible externally.
    if (nss.isTimeseriesBucketsCollection()) {
        _nss = nss.getTimeseriesViewNamespace();
        return;
    }

    _nss = nss;
}

void BackupBlock::_setUuid(OperationContext* opCtx, DurableCatalog* catalog, RecordId catalogId) {
    // Caller controls lifetime of catalog and relevant lock
    std::shared_ptr<BSONCollectionCatalogEntry::MetaData> md =
        catalog->getMetaData(opCtx, catalogId);
    _uuid = md->options.uuid;
}

void BackupBlock::_initialize(OperationContext* opCtx,
                              boost::optional<Timestamp> checkpointTimestamp) {
    if (!opCtx) {
        return;
    }

    {
        // Fetch the latest values for the ident.
        Lock::GlobalLock lk(opCtx, MODE_IS);
        DurableCatalog* catalog = DurableCatalog::get(opCtx);
        std::vector<DurableCatalog::Entry> catalogEntries = catalog->getAllCatalogEntries(opCtx);
        for (const DurableCatalog::Entry& e : catalogEntries) {
            if (StringData(_filenameStem).startsWith("index-"_sd) &&
                // Index idents will get the namespace and UUID of their respective collection.
                catalog->isIndexInEntry(opCtx, e.catalogId, _filenameStem)) {
                _setUuid(opCtx, catalog, e.catalogId);
                _setNamespaceString(e.tenantNs.getNss());
                break;
            }

            if (e.ident == _filenameStem) {
                // This ident represents the collection.
                _setUuid(opCtx, catalog, e.catalogId);
                _setNamespaceString(e.tenantNs.getNss());
                break;
            }
        }
    }

    if (!checkpointTimestamp) {
        return;
    }

    // Check if the ident had a different value at the checkpoint timestamp. If so, we want to use
    // that instead as that will be the ident's value when restoring from the backup.
    boost::optional<std::pair<NamespaceString, UUID>> historicalEntry =
        HistoricalIdentTracker::get(opCtx).lookup(_filenameStem, checkpointTimestamp.get());
    if (historicalEntry) {
        _uuid = historicalEntry->second;
        _setNamespaceString(historicalEntry->first);
    }
}

}  // namespace mongo
