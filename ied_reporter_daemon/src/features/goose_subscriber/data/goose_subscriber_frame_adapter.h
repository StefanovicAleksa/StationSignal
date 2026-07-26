#ifndef GOOSE_SUBSCRIBER_FRAME_ADAPTER_H_
#define GOOSE_SUBSCRIBER_FRAME_ADAPTER_H_

#include "goose_subscriber.h"
#include "features/goose_subscriber/domain/goose_subscriber_types.h"

/*
 * The ONLY file in this feature that touches the opaque GooseSubscriber type
 * inside a GooseListener callback. Installed as the GooseListener on every
 * GooseSubscriber (see data/goose_subscriber_connection.c). Extracts every
 * field it needs before returning - GooseSubscriber_getDataSetValues is
 * documented (goose_subscriber.h) as only safe to use inside the callback
 * when the receiver runs in a separate thread - then hands an owned
 * GooseSubscriberRecord (built via goose_subscriber_usecases) to the user's
 * registered record callback.
 *
 * Fires on GooseReceiver's internal reception thread (GooseReceiver_start()).
 * Unlike ReportCallbackFunction, goose_subscriber.h documents no explicit
 * deadlock warning for GooseListener - be conservative anyway: must not
 * block, must not call back into this feature's own API from here.
 */
void
GooseSubscriberFrameAdapter_onGooseReceived(GooseSubscriber subscriber, void* parameter);

#endif /* GOOSE_SUBSCRIBER_FRAME_ADAPTER_H_ */
