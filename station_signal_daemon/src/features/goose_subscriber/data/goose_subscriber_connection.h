#ifndef GOOSE_SUBSCRIBER_CONNECTION_H_
#define GOOSE_SUBSCRIBER_CONNECTION_H_

#include "features/goose_subscriber/domain/goose_subscriber_types.h"

/*
 * All GooseReceiver/GooseSubscriber third-party integration for this feature
 * lives here: receiver/subscriber creation, the liveness polling thread, and
 * start/stop/destroy lifecycle. Deliberately not unit-tested in depth -
 * proven E2E, mirroring mms_report_client_connection's convention (a live
 * GooseReceiver can't be meaningfully faked in a hermetic unit test).
 *
 * GOOSE has no association/connection to reconnect - unlike
 * mms_report_client_connection's reconnect-supervisor-thread, there is
 * nothing to (re)establish here. The one thread this file owns is a liveness
 * POLLER, not a reconnect loop: GOOSE gives no push signal for "a publisher
 * stopped sending" (no equivalent of IedConnection's state-changed handler),
 * so staleness can only be detected by periodically checking
 * GooseSubscriber_isValid(). This is a deliberate, narrow, approved exception
 * to CLAUDE.md's "no cyclic polling" rule - see the top comment on the
 * liveness thread below for why it doesn't violate the rule's intent.
 */

/* Builds the GooseReceiver, one GooseSubscriber per handle->targetEntries[]
 * entry (applying setDstMac/setAppId filters when target->hasAddress),
 * installs GooseSubscriberFrameAdapter_onGooseReceived as each subscriber's
 * listener, and adds every subscriber to the receiver - all before
 * GooseReceiver_start(), per GooseReceiver_addSubscriber's documented "must
 * not be called while running" constraint. Does not start the receiver or
 * the liveness thread yet. */
GooseSubscriberError
GooseSubscriberConnection_create(GooseSubscriberHandle handle);

/* Calls GooseReceiver_start() (library-internal reception thread - no manual
 * tick() polling), then starts the liveness thread. Non-blocking. */
GooseSubscriberError
GooseSubscriberConnection_start(GooseSubscriberHandle handle);

/* Stops the liveness thread (bounded, prompt - chunked sleep, not a full poll
 * cycle), then GooseReceiver_stop(). Blocking. MUST be called from the
 * caller's own thread, never from within a registered callback (deadlock
 * risk). Safe to call more than once. */
void
GooseSubscriberConnection_stop(GooseSubscriberHandle handle);

/* Destroys the GooseReceiver - cascades to destroy every attached
 * GooseSubscriber (goose_receiver.h). Call after _stop(). */
void
GooseSubscriberConnection_destroy(GooseSubscriberHandle handle);

#endif /* GOOSE_SUBSCRIBER_CONNECTION_H_ */
