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

#include "mongo/platform/basic.h"

#include "mongo/db/repl/transaction_oplog_application.h"

#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/repl/apply_ops.h"
#include "mongo/db/session_catalog_mongod.h"
#include "mongo/db/transaction_history_iterator.h"
#include "mongo/db/transaction_participant.h"

namespace mongo {

/**
 * Helper that will find the previous oplog entry for that transaction, transform it to be a normal
 * applyOps command and applies the oplog entry. Currently used for oplog application of a
 * commitTransaction oplog entry during recovery, rollback and initial sync.
 */
Status _applyTransactionFromOplogChain(OperationContext* opCtx,
                                       const repl::OplogEntry& entry,
                                       repl::OplogApplication::Mode mode) {
    invariant(mode == repl::OplogApplication::Mode::kRecovering ||
              mode == repl::OplogApplication::Mode::kInitialSync);

    BSONObj prepareCmd;
    {
        // Traverse the oplog chain with its own snapshot and read timestamp.
        ReadSourceScope readSourceScope(opCtx);

        // Get the corresponding prepareTransaction oplog entry.
        const auto prepareOpTime = entry.getPrevWriteOpTimeInTransaction();
        invariant(prepareOpTime);
        TransactionHistoryIterator iter(prepareOpTime.get());
        invariant(iter.hasNext());
        const auto prepareOplogEntry = iter.next(opCtx);

        // Transform prepare command into a normal applyOps command.
        prepareCmd = prepareOplogEntry.getOperationToApply().removeField("prepare");
    }
    BSONObjBuilder resultWeDontCareAbout;
    return applyOps(
        opCtx, entry.getNss().db().toString(), prepareCmd, mode, &resultWeDontCareAbout);
}

Status applyCommitTransaction(OperationContext* opCtx,
                              const repl::OplogEntry& entry,
                              repl::OplogApplication::Mode mode) {
    // Return error if run via applyOps command.
    uassert(50987,
            "commitTransaction is only used internally by secondaries.",
            mode != repl::OplogApplication::Mode::kApplyOpsCmd);

    IDLParserErrorContext ctx("commitTransaction");
    auto commitCommand = CommitTransactionOplogObject::parse(ctx, entry.getObject());
    const bool prepared = !commitCommand.getPrepared() || *commitCommand.getPrepared();
    if (!prepared)
        return Status::OK();

    if (mode == repl::OplogApplication::Mode::kRecovering ||
        mode == repl::OplogApplication::Mode::kInitialSync) {
        return _applyTransactionFromOplogChain(opCtx, entry, mode);
    }

    invariant(mode == repl::OplogApplication::Mode::kSecondary);

    // Transaction operations are in its own batch, so we can modify their opCtx.
    invariant(entry.getSessionId());
    invariant(entry.getTxnNumber());
    opCtx->setLogicalSessionId(*entry.getSessionId());
    opCtx->setTxnNumber(*entry.getTxnNumber());
    // The write on transaction table may be applied concurrently, so refreshing state
    // from disk may read that write, causing starting a new transaction on an existing
    // txnNumber. Thus, we start a new transaction without refreshing state from disk.
    MongoDOperationContextSessionWithoutRefresh sessionCheckout(opCtx);

    auto transaction = TransactionParticipant::get(opCtx);
    invariant(transaction);
    transaction.unstashTransactionResources(opCtx, "commitTransaction");
    invariant(commitCommand.getCommitTimestamp());
    transaction.commitPreparedTransaction(
        opCtx, *commitCommand.getCommitTimestamp(), entry.getOpTime());
    return Status::OK();
}

Status applyAbortTransaction(OperationContext* opCtx,
                             const repl::OplogEntry& entry,
                             repl::OplogApplication::Mode mode) {
    // Return error if run via applyOps command.
    uassert(50972,
            "abortTransaction is only used internally by secondaries.",
            mode != repl::OplogApplication::Mode::kApplyOpsCmd);

    // We don't put transactions into the prepare state until the end of recovery, so there is
    // no transaction to abort.
    if (mode == repl::OplogApplication::Mode::kRecovering) {
        return Status::OK();
    }

    // TODO: SERVER-36492 Only run on secondary until we support initial sync.
    invariant(mode == repl::OplogApplication::Mode::kSecondary);

    // Transaction operations are in its own batch, so we can modify their opCtx.
    invariant(entry.getSessionId());
    invariant(entry.getTxnNumber());
    opCtx->setLogicalSessionId(*entry.getSessionId());
    opCtx->setTxnNumber(*entry.getTxnNumber());
    // The write on transaction table may be applied concurrently, so refreshing state
    // from disk may read that write, causing starting a new transaction on an existing
    // txnNumber. Thus, we start a new transaction without refreshing state from disk.
    MongoDOperationContextSessionWithoutRefresh sessionCheckout(opCtx);

    auto transaction = TransactionParticipant::get(opCtx);
    transaction.unstashTransactionResources(opCtx, "abortTransaction");
    transaction.abortActiveTransaction(opCtx);
    return Status::OK();
}

repl::MultiApplier::Operations readTransactionOperationsFromOplogChain(
    OperationContext* opCtx,
    const repl::OplogEntry& commitOrPrepare,
    const std::vector<repl::OplogEntry*> cachedOps) {
    repl::MultiApplier::Operations ops;

    // Get the previous oplog entry.
    auto currentOpTime = commitOrPrepare.getOpTime();

    // The cachedOps are the ops for this transaction that are from the same oplog application batch
    // as the commit or prepare, those which have not necessarily been written to the oplog.  These
    // ops are in order of increasing timestamp.

    // The lastEntryOpTime is the OpTime of the last (latest OpTime) entry for this transaction
    // which is expected to be present in the oplog.  It is the entry before the first cachedOp,
    // unless there are no cachedOps in which case it is the entry before the commit or prepare.
    const auto lastEntryOpTime = (cachedOps.empty() ? commitOrPrepare : *cachedOps.front())
                                     .getPrevWriteOpTimeInTransaction();
    invariant(lastEntryOpTime < currentOpTime);

    TransactionHistoryIterator iter(lastEntryOpTime.get());
    // Empty commits are not allowed, but empty prepares are.
    invariant(commitOrPrepare.getCommandType() !=
                  repl::OplogEntry::CommandType::kCommitTransaction ||
              !cachedOps.empty() || iter.hasNext());
    auto commitOrPrepareObj = commitOrPrepare.toBSON();

    // First retrieve and transform the ops from the oplog, which will be retrieved in reverse
    // order.
    while (iter.hasNext()) {
        const auto& operationEntry = iter.next(opCtx);
        invariant(operationEntry.isInPendingTransaction());
        // Now reconstruct the entry "as if" it were at the commit or prepare time.
        BSONObjBuilder builder(operationEntry.getReplOperation().toBSON());
        builder.appendElementsUnique(commitOrPrepareObj);
        ops.emplace_back(builder.obj());
    }
    std::reverse(ops.begin(), ops.end());

    // Next retrieve and transform the ops from the current batch, which are in increasing timestamp
    // order.
    for (auto* cachedOp : cachedOps) {
        const auto& operationEntry = *cachedOp;
        invariant(operationEntry.isInPendingTransaction());
        // Now reconstruct the entry "as if" it were at the commit or prepare time.
        BSONObjBuilder builder(operationEntry.getReplOperation().toBSON());
        builder.appendElementsUnique(commitOrPrepareObj);
        ops.emplace_back(builder.obj());
    }
    return ops;
}

}  // namespace mongo
