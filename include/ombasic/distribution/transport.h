#ifndef OMBASIC_DISTRIBUTION_TRANSPORT_H
#define OMBASIC_DISTRIBUTION_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

#include "ombasic/distribution/protocol.h"
#include "ombasic/distribution/handshake.h"

typedef struct Om_Distribution_Listener {
    void *owned_state;
} Om_Distribution_Listener;

typedef struct Om_Distribution_Connection {
    void *owned_state;
} Om_Distribution_Connection;

/* Opens a blocking TCP listener. A port of zero requests an ephemeral port. */
Om_Status om_distribution_listener_open(
    Om_Distribution_Listener *listener,
    Om_String_View bind_address,
    uint16_t port,
    int backlog
);
uint16_t om_distribution_listener_port(
    const Om_Distribution_Listener *listener
);
Om_Status om_distribution_listener_accept(
    const Om_Distribution_Listener *listener,
    Om_Distribution_Connection *connection
);
Om_Status om_distribution_connection_connect(
    Om_Distribution_Connection *connection,
    Om_String_View host,
    uint16_t port
);
/* Both peers send HELLO, validate compatibility, then exchange HELLO_ACK. */
Om_Status om_distribution_connection_handshake(
    Om_Distribution_Connection *connection,
    const Om_Distribution_Node_Handshake *local,
    const Om_Distribution_Node_Id *expected_peer,
    size_t maximum_frame_bytes
);
/* Same exchange, and returns the validated peer handshake on success. */
Om_Status om_distribution_connection_handshake_peer(
    Om_Distribution_Connection *connection,
    const Om_Distribution_Node_Handshake *local,
    const Om_Distribution_Node_Id *expected_peer,
    size_t maximum_frame_bytes,
    Om_Distribution_Node_Handshake *remote
);

/* Send calls on one connection must be externally serialized. */
Om_Status om_distribution_connection_send(
    Om_Distribution_Connection *connection,
    const Om_Distribution_Frame *frame,
    size_t maximum_frame_bytes
);

/* Payload bytes are borrowed until the next receive or connection close. */
Om_Status om_distribution_connection_receive(
    Om_Distribution_Connection *connection,
    size_t maximum_frame_bytes,
    Om_Distribution_Frame *frame
);

/* Interrupts blocked I/O without releasing the connection's storage. */
void om_distribution_connection_interrupt(
    Om_Distribution_Connection *connection
);
void om_distribution_connection_close(
    Om_Distribution_Connection *connection
);
void om_distribution_listener_close(Om_Distribution_Listener *listener);

#endif
