// Copyright (c) 2013, Cloudera, inc.

#include <glog/logging.h>

#include "kudu/tablet/transactions/alter_schema_transaction.h"
#include "kudu/tablet/transactions/write_util.h"

#include "kudu/common/wire_protocol.h"
#include "kudu/rpc/rpc_context.h"
#include "kudu/server/hybrid_clock.h"
#include "kudu/tablet/tablet.h"
#include "kudu/tablet/tablet_peer.h"
#include "kudu/tablet/tablet_metrics.h"
#include "kudu/tserver/tserver.pb.h"
#include "kudu/util/trace.h"

namespace kudu {
namespace tablet {

using boost::bind;
using boost::shared_lock;
using consensus::ReplicateMsg;
using consensus::CommitMsg;
using consensus::OP_ABORT;
using consensus::ALTER_SCHEMA_OP;
using consensus::DriverType;
using strings::Substitute;
using tserver::TabletServerErrorPB;
using tserver::AlterSchemaRequestPB;
using tserver::AlterSchemaResponsePB;

string AlterSchemaTransactionState::ToString() const {
  return Substitute("AlterSchemaTransactionState "
                    "[timestamp=$0, schema=$1, request=$2]",
                    timestamp().ToString(),
                    schema_ == NULL ? "(none)" : schema_->ToString(),
                    request_ == NULL ? "(none)" : request_->ShortDebugString());
}

void AlterSchemaTransactionState::AcquireSchemaLock(boost::shared_mutex* l) {
  TRACE("Acquiring schema lock in exclusive mode");
  schema_lock_ = boost::unique_lock<boost::shared_mutex>(*l);
  TRACE("Acquired schema lock");
}

void AlterSchemaTransactionState::ReleaseSchemaLock() {
  CHECK(schema_lock_.owns_lock());
  schema_lock_ = boost::unique_lock<boost::shared_mutex>();
  TRACE("Released schema lock");
}


AlterSchemaTransaction::AlterSchemaTransaction(AlterSchemaTransactionState* state,
                                               DriverType type)
    : Transaction(state, type, Transaction::ALTER_SCHEMA_TXN),
      state_(state) {
}

void AlterSchemaTransaction::NewReplicateMsg(gscoped_ptr<ReplicateMsg>* replicate_msg) {
  replicate_msg->reset(new ReplicateMsg);
  (*replicate_msg)->set_op_type(ALTER_SCHEMA_OP);
  (*replicate_msg)->mutable_alter_schema_request()->CopyFrom(*state()->request());
}

Status AlterSchemaTransaction::Prepare() {
  TRACE("PREPARE ALTER-SCHEMA: Starting");

  // Decode schema
  gscoped_ptr<Schema> schema(new Schema);
  Status s = SchemaFromPB(state_->request()->schema(), schema.get());
  if (!s.ok()) {
    state_->completion_callback()->set_error(s, TabletServerErrorPB::INVALID_SCHEMA);
    return s;
  }

  Tablet* tablet = state_->tablet_peer()->tablet();
  RETURN_NOT_OK(tablet->CreatePreparedAlterSchema(state(), schema.get()));

  state_->AddToAutoReleasePool(schema.release());

  TRACE("PREPARE ALTER-SCHEMA: finished");
  return s;
}

Status AlterSchemaTransaction::Start() {
  state_->set_timestamp(state_->tablet_peer()->clock()->Now());
  TRACE("START. Timestamp: $0", server::HybridClock::GetPhysicalValue(state_->timestamp()));
  return Status::OK();
}

void AlterSchemaTransaction::NewCommitAbortMessage(gscoped_ptr<CommitMsg>* commit_msg) {
  commit_msg->reset(new CommitMsg());
  (*commit_msg)->set_op_type(OP_ABORT);
  (*commit_msg)->mutable_alter_schema_response()->CopyFrom(*state_->response());
}

Status AlterSchemaTransaction::Apply(gscoped_ptr<CommitMsg>* commit_msg) {
  TRACE("APPLY ALTER-SCHEMA: Starting");

  Tablet* tablet = state_->tablet_peer()->tablet();
  RETURN_NOT_OK(tablet->AlterSchema(state()));

  commit_msg->reset(new CommitMsg());
  (*commit_msg)->set_op_type(ALTER_SCHEMA_OP);
  (*commit_msg)->set_timestamp(state_->timestamp().ToUint64());
  return Status::OK();
}

void AlterSchemaTransaction::Finish() {
  // Now that all of the changes have been applied and the commit is durable
  // make the changes visible to readers.
  TRACE("AlterSchemaCommitCallback: making edits visible");
  state()->commit();
}

string AlterSchemaTransaction::ToString() const {
  return Substitute("AlterSchemaTransaction [state=$0]", state_->ToString());
}

}  // namespace tablet
}  // namespace kudu