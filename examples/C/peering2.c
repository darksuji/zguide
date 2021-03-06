//
//  Broker peering simulation (part 2)
//  Prototypes the request-reply flow
//
//  While this example runs in a single process, that is just to make
//  it easier to start and stop the example. Each thread has its own
//  context and conceptually acts as a separate process.
//
//  Changes for 2.1:
//  - added version assertion
//  - use separate contexts for each thread
//  - use ipc:// instead of inproc://
//  - close sockets in each child thread
//  - call zmq_term in each thread before ending
//
#include "zmsg.c"

#define NBR_CLIENTS 10
#define NBR_WORKERS 3

//  A simple dequeue operation for queue implemented as array
#define DEQUEUE(q) memmove (&(q)[0], &(q)[1], sizeof (q) - sizeof (q [0]))

//  Request-reply client using REQ socket
//
static void *
client_thread (void *args) {
    void *context = zmq_init (1);
    void *client = zmq_socket (context, ZMQ_REQ);
    zmq_connect (client, "ipc://localfe.ipc");

    zmsg_t *zmsg = zmsg_new ();
    while (1) {
        //  Send request, get reply
        zmsg_body_set (zmsg, "HELLO");
        zmsg_send (&zmsg, client);
        zmsg = zmsg_recv (client);
        printf ("I: client status: %s\n", zmsg_body (zmsg));
    }
    //  We never get here but if we did, this is how we'd exit cleanly
    zmq_close (client);
    zmq_term (context);
    return (NULL);
}

//  Worker using REQ socket to do LRU routing
//
static void *
worker_thread (void *args) {
    void *context = zmq_init (1);
    void *worker = zmq_socket (context, ZMQ_REQ);
    zmq_connect (worker, "ipc://localbe.ipc");

    //  Tell broker we're ready for work
    zmsg_t *zmsg = zmsg_new ();
    zmsg_body_set (zmsg, "READY");
    zmsg_send (&zmsg, worker);

    while (1) {
        zmsg = zmsg_recv (worker);
        //  Do some 'work'
        sleep (1);
        zmsg_body_fmt (zmsg, "OK - %04x", randof (0x10000));
        zmsg_send (&zmsg, worker);
    }
    //  We never get here but if we did, this is how we'd exit cleanly
    zmq_close (worker);
    zmq_term (context);
    return (NULL);
}


int main (int argc, char *argv [])
{
    //  First argument is this broker's name
    //  Other arguments are our peers' names
    //
    s_version_assert (2, 1);
    if (argc < 2) {
        printf ("syntax: peering2 me {you}...\n");
        exit (EXIT_FAILURE);
    }
    char *self = argv [1];
    printf ("I: preparing broker at %s...\n", self);
    srandom ((unsigned) time (NULL));

    //  Prepare our context and sockets
    void *context = zmq_init (1);
    char endpoint [256];

    //  Bind cloud frontend to endpoint
    void *cloudfe = zmq_socket (context, ZMQ_XREP);
    snprintf (endpoint, 255, "ipc://%s-cloud.ipc", self);
    zmq_setsockopt (cloudfe, ZMQ_IDENTITY, self, strlen (self));
    int rc = zmq_bind (cloudfe, endpoint);
    assert (rc == 0);

    //  Connect cloud backend to all peers
    void *cloudbe = zmq_socket (context, ZMQ_XREP);
    zmq_setsockopt (cloudbe, ZMQ_IDENTITY, self, strlen (self));

    int argn;
    for (argn = 2; argn < argc; argn++) {
        char *peer = argv [argn];
        printf ("I: connecting to cloud frontend at '%s'\n", peer);
        snprintf (endpoint, 255, "ipc://%s-cloud.ipc", peer);
        rc = zmq_connect (cloudbe, endpoint);
        assert (rc == 0);
    }

    //  Prepare local frontend and backend
    void *localfe = zmq_socket (context, ZMQ_XREP);
    zmq_bind (localfe, "ipc://localfe.ipc");
    void *localbe = zmq_socket (context, ZMQ_XREP);
    zmq_bind (localbe, "ipc://localbe.ipc");

    //  Get user to tell us when we can start...
    printf ("Press Enter when all brokers are started: ");
    getchar ();

    //  Start local workers
    int worker_nbr;
    for (worker_nbr = 0; worker_nbr < NBR_WORKERS; worker_nbr++) {
        pthread_t worker;
        pthread_create (&worker, NULL, worker_thread, NULL);
    }
    //  Start local clients
    int client_nbr;
    for (client_nbr = 0; client_nbr < NBR_CLIENTS; client_nbr++) {
        pthread_t client;
        pthread_create (&client, NULL, client_thread, NULL);
    }

    //  Interesting part
    //  -------------------------------------------------------------
    //  Request-reply flow
    //  - Poll backends and process local/cloud replies
    //  - While worker available, route localfe to local or cloud

    //  Queue of available workers
    int capacity = 0;
    char *worker_queue [NBR_WORKERS];

    while (1) {
        zmq_pollitem_t backends [] = {
            { localbe, 0, ZMQ_POLLIN, 0 },
            { cloudbe, 0, ZMQ_POLLIN, 0 }
        };
        //  If we have no workers anyhow, wait indefinitely
        rc = zmq_poll (backends, 2, capacity? 1000000: -1);
        assert (rc >= 0);

        //  Handle reply from local worker
        zmsg_t *zmsg = NULL;
        if (backends [0].revents & ZMQ_POLLIN) {
            zmsg = zmsg_recv (localbe);

            assert (capacity < NBR_WORKERS);
            //  Use worker address for LRU routing
            worker_queue [capacity++] = zmsg_unwrap (zmsg);
            if (strcmp (zmsg_address (zmsg), "READY") == 0)
                zmsg_destroy (&zmsg);   //  Don't route it
        }
        //  Or handle reply from peer broker
        else
        if (backends [1].revents & ZMQ_POLLIN) {
            zmsg = zmsg_recv (cloudbe);
            //  We don't use peer broker address for anything
            free (zmsg_unwrap (zmsg));
        }
        //  Route reply to cloud if it's addressed to a broker
        for (argn = 2; zmsg && argn < argc; argn++) {
            if (strcmp (zmsg_address (zmsg), argv [argn]) == 0)
                zmsg_send (&zmsg, cloudfe);
        }
        //  Route reply to client if we still need to
        if (zmsg)
            zmsg_send (&zmsg, localfe);

        //  Now route as many clients requests as we can handle
        //
        while (capacity) {
            zmq_pollitem_t frontends [] = {
                { localfe, 0, ZMQ_POLLIN, 0 },
                { cloudfe, 0, ZMQ_POLLIN, 0 }
            };
            rc = zmq_poll (frontends, 2, 0);
            assert (rc >= 0);
            int reroutable = 0;
            //  We'll do peer brokers first, to prevent starvation
            if (frontends [1].revents & ZMQ_POLLIN) {
                zmsg = zmsg_recv (cloudfe);
                reroutable = 0;
            }
            else
            if (frontends [0].revents & ZMQ_POLLIN) {
                zmsg = zmsg_recv (localfe);
                reroutable = 1;
            }
            else
                break;      //  No work, go back to backends

            //  If reroutable, send to cloud 20% of the time
            //  Here we'd normally use cloud status information
            //
            if (reroutable && argc > 2 && randof (5) == 0) {
                //  Route to random broker peer
                int random_peer = randof (argc - 2) + 2;
                zmsg_wrap (zmsg, argv [random_peer], NULL);
                zmsg_send (&zmsg, cloudbe);
            }
            else {
                zmsg_wrap (zmsg, worker_queue [0], "");
                zmsg_send (&zmsg, localbe);

                //  Dequeue and drop the next worker address
                free (worker_queue [0]);
                DEQUEUE (worker_queue);
                capacity--;
            }
        }
    }
    //  We never get here but clean up anyhow
    zmq_close (localbe);
    zmq_close (cloudbe);
    zmq_term (context);
    return EXIT_SUCCESS;
}
