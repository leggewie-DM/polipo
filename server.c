/*
Copyright (c) 2003, 2004 by Juliusz Chroboczek

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#include "polipo.h"

int serverExpireTime =  24 * 60 * 60;
int smallRequestTime = 10;
int replyUnpipelineTime = 20;
int replyUnpipelineSize = 1024 * 1024;
int pipelineAdditionalRequests = 1;
int maxPipelineTrain = 10;
AtomPtr parentHost = NULL;
int parentPort = 8123;
int pmmFirstSize = 0, pmmSize = 0;
int serverSlots = 2;
int serverMaxSlots = 4;

static HTTPServerPtr servers = 0;

static int httpServerContinueObjectHandler(int, ObjectHandlerPtr);
static void httpServerDelayedFinish(HTTPConnectionPtr);

void
preinitServer(void)
{
    CONFIG_VARIABLE(parentHost, CONFIG_ATOM_LOWER, "Parent proxy hostname.");
    CONFIG_VARIABLE(parentPort, CONFIG_INT, "Parent proxy port.");
    CONFIG_VARIABLE(serverExpireTime, CONFIG_TIME,
                    "Time during which server data is valid.");
    CONFIG_VARIABLE(smallRequestTime, CONFIG_TIME,
                    "Estimated time for a small request.");
    CONFIG_VARIABLE(replyUnpipelineTime, CONFIG_TIME,
                    "Estimated time for a pipeline break.");
    CONFIG_VARIABLE(replyUnpipelineSize, CONFIG_INT,
                    "Size for a pipeline break.");
    CONFIG_VARIABLE(pipelineAdditionalRequests, CONFIG_TRISTATE,
                    "Pipeline requests on an active connection.");
    CONFIG_VARIABLE(maxPipelineTrain, CONFIG_INT,
                    "Maximum number of requests pipelined at a time.");
    CONFIG_VARIABLE(pmmFirstSize, CONFIG_INT,
                    "The size of the first PMM chunk.");
    CONFIG_VARIABLE(pmmSize, CONFIG_INT,
                    "The size of a PMM chunk.");
    CONFIG_VARIABLE(serverSlots, CONFIG_INT,
                    "Maximum number of connections per server.");
    CONFIG_VARIABLE(serverMaxSlots, CONFIG_INT,
                    "Maximum number of connections per broken server.");
}

static void
discardServer(HTTPServerPtr server)
{
    HTTPServerPtr previous;
    assert(!server->request);

    if(server == servers)
        servers = server->next;
    else {
        previous = servers;
        while(previous->next != server)
            previous = previous->next;
        previous->next = server->next;
    }

    free(server);
}

static int
httpServerIdle(HTTPServerPtr server)
{
    int i;
    if(server->request) 
        return 0;
    for(i = 0; i < server->maxslots; i++)
        if(server->connection[i])
            return 0;
    return 1;
}

static int
expireServersHandler(TimeEventHandlerPtr event)
{
    HTTPServerPtr server, next;
    TimeEventHandlerPtr e;
    server = servers;
    while(server) {
        next = server->next;
        if(httpServerIdle(server) &&
           server->time + serverExpireTime < current_time.tv_sec)
            discardServer(server);
        server = next;
    }
    e = scheduleTimeEvent(serverExpireTime / 60 + 60, 
                          expireServersHandler, 0, NULL);
    if(!e) {
        do_log(L_ERROR, "Couldn't schedule server expiry.\n");
        polipoExit();
    }
    return 1;
}

static int
roundSize(int size)
{
    if(size < CHUNK_SIZE)
        return 1 << log2_ceil(pmmSize);
    else if(size > CHUNK_SIZE)
        return (size + CHUNK_SIZE - 1) / CHUNK_SIZE * CHUNK_SIZE;
    else
        return size;
}
        

void
initServer(void)
{
    TimeEventHandlerPtr event;
    servers = NULL;

    if(pmmFirstSize || pmmSize) {
        if(pmmSize == 0) pmmSize = pmmFirstSize;
        if(pmmFirstSize == 0) pmmFirstSize = pmmSize;
        pmmSize = roundSize(pmmSize);
        pmmFirstSize = roundSize(pmmFirstSize);
    }

    if(serverMaxSlots < 1)
        serverMaxSlots = 1;
    if(serverSlots < 1)
        serverSlots = 1;
    if(serverSlots > serverMaxSlots)
        serverSlots = serverMaxSlots;

    event = scheduleTimeEvent(serverExpireTime / 60 + 60, expireServersHandler,
                              0, NULL);
    if(event == NULL) {
        do_log(L_ERROR, "Couldn't schedule server expiry.\n");
        exit(1);
    }
}

static HTTPServerPtr
getServer(char *name, int port, int proxy)
{
    HTTPServerPtr server;
    int i;

    server = servers;
    while(server) {
        if(strcmp(server->name, name) == 0 && server->port == port &&
           server->isProxy == proxy) {
            if(httpServerIdle(server) &&
               server->time +  serverExpireTime < current_time.tv_sec) {
                discardServer(server);
                server = NULL;
                break;
            } else {
                server->time = current_time.tv_sec;
                return server;
            }
        }
        server = server->next;
    }
    
    server = malloc(sizeof(HTTPServerRec));
    if(server == NULL) {
        do_log(L_ERROR, "Couldn't allocate server.\n");
        return NULL;
    }

    server->connection = malloc(serverMaxSlots * sizeof(HTTPConnectionPtr));
    if(server->connection == NULL) {
        do_log(L_ERROR, "Couldn't allocate server.\n");
        free(server);
        return NULL;
    }

    server->idleHandler = malloc(serverMaxSlots * sizeof(FdEventHandlerPtr));
    if(server->connection == NULL) {
        do_log(L_ERROR, "Couldn't allocate server.\n");
        free(server->connection);
        free(server);
        return NULL;
    }

    server->maxslots = serverMaxSlots;

    server->name = strdup(name);
    if(server->name == NULL) {
        do_log(L_ERROR, "Couldn't allocate server name.\n");
        free(server);
        return NULL;
    }

    server->port = port;
    server->addrindex = 0;
    server->isProxy = proxy;
    server->version = HTTP_UNKNOWN;
    server->persistent = 0;
    server->pipeline = 0;
    server->time = current_time.tv_sec;
    server->rtt = -1;
    server->rate = -1;
    server->numslots = MIN(serverSlots, server->maxslots);
    for(i = 0; i < server->maxslots; i++) {
        server->connection[i] = NULL;
        server->idleHandler[i] = NULL;
    }
    server->request = NULL;
    server->request_last = NULL;
    server->lies = 0;

    server->next = servers;
    servers = server;
    return server;
}

int
httpServerQueueRequest(HTTPServerPtr server, HTTPRequestPtr request)
{
    assert(request->request && request->request->request == request);
    assert(request->connection == NULL);
    if(server->request) {
        server->request_last->next = request;
        server->request_last = request;
    } else {
        server->request_last = request;
        server->request = request;
    }
    return 1;
}

void
httpServerAbort(HTTPConnectionPtr connection, int fail,
                int code, AtomPtr message)
{
    HTTPRequestPtr request = connection->request;
    if(request) {
        if(request->request) {
            httpClientError(request->request, code, retainAtom(message));
        }
        if(fail) {
            request->object->flags |= OBJECT_FAILED;
            if(request->object->flags & OBJECT_INITIAL)
                abortObject(request->object, code, retainAtom(message));
            notifyObject(request->object);
        }
    }
    releaseAtom(message);
    if(!connection->connecting)
        httpServerFinish(connection, 1, 0);
}

void
httpServerAbortRequest(HTTPRequestPtr request, int fail,
                       int code, AtomPtr message)
{
    if(request->connection && request == request->connection->request) {
        httpServerAbort(request->connection, fail, code, message);
    } else {
        HTTPRequestPtr requestor = request->request;
        if(requestor) {
            requestor->request = NULL;
            request->request = NULL;
            httpClientError(requestor, code, retainAtom(message));
        }
        if(fail) {
            request->object->flags |= OBJECT_FAILED;
            if(request->object->flags & OBJECT_INITIAL)
                abortObject(request->object, code, retainAtom(message));
            notifyObject(request->object);
        }
        releaseAtom(message);
    }
}

void 
httpServerClientReset(HTTPRequestPtr request)
{
    if(request->connection && 
       request->connection->fd >= 0 &&
       !request->connection->connecting &&
       request->connection->request == request)
        pokeFdEvent(request->connection->fd, -ECLIENTRESET, POLLIN | POLLOUT);
}


int
httpMakeServerRequest(char *name, int port, ObjectPtr object, 
                  int method, int from, int to, HTTPRequestPtr requestor)
{
    HTTPServerPtr server;
    HTTPRequestPtr request;
    int rc;

    assert(!(object->flags & OBJECT_INPROGRESS));

    if(parentHost) {
        server = getServer(parentHost->string, parentPort, 1);
    } else {
        server = getServer(name, port, 0);
    }
    if(server == NULL) return -1;

    object->flags |= OBJECT_INPROGRESS;
    object->requestor = requestor;

    request = httpMakeRequest();
    if(!request) {
        do_log(L_ERROR, "Couldn't allocate request.\n");
        return -1;
    }

    /* Because we allocate objects in chunks, we cannot have data that
       doesn't start at a chunk boundary. */
    if(from % CHUNK_SIZE != 0) {
        objectFillFromDisk(object, from / CHUNK_SIZE * CHUNK_SIZE, 1);
        if(objectHoleSize(object, from - 1) != 0)
            from = from / CHUNK_SIZE * CHUNK_SIZE;
    }

    request->object = retainObject(object);
    request->method = method;
    if(method == METHOD_CONDITIONAL_GET) {
        if(server->lies > 0)
            request->method = METHOD_HEAD;
    }
    request->from = from;
    request->to = to;
    request->persistent = 1;
    request->request = requestor;
    requestor->request = request;
    request->cache_control = requestor->cache_control;
    request->time0 = null_time;
    request->time1 = null_time;
    request->wait_continue = expectContinue ? requestor->wait_continue : 0;

    rc = httpServerQueueRequest(server, request);
    if(rc < 0) {
        do_log(L_ERROR, "Couldn't queue request.\n");
        request->request = NULL;
        requestor->request = NULL;
        object->flags &= ~(OBJECT_INPROGRESS | OBJECT_VALIDATING);
        releaseNotifyObject(object);
        httpDestroyRequest(request);
        return 1;
    }

    if(request->wait_continue) {
        if(server->version == HTTP_10) {
            httpServerAbortRequest(request, 1,
                                   417, internAtom("Expectation failed"));
            return 1;
        }
    } else if(expectContinue >= 2 && server->version == HTTP_11) {
        if(request->method == METHOD_POST || request->method == METHOD_PUT)
            request->wait_continue = 1;
    }
        
 again:
    rc = httpServerTrigger(server);
    if(rc < 0) {
        /* We must be very short on memory.  If there are any requests
           queued, we abort one and try again.  If there aren't, we
           give up. */
        do_log(L_ERROR, "Couldn't trigger server -- out of memory?\n");
        if(server->request) {
            httpServerAbortRequest(server->request, 1, 503,
                                   internAtom("Couldn't trigger server"));
            goto again;
        }
    }
    return 1;
}

int
httpServerConnection(HTTPServerPtr server)
{
    HTTPConnectionPtr connection;
    int i;

    connection = httpMakeConnection();
    if(connection == NULL) {
        do_log(L_ERROR, "Couldn't allocate server connection.\n");
        return -1;
    }
    connection->server = server;

    for(i = 0; i < server->numslots; i++) {
        if(!server->connection[i]) {
            server->connection[i] = connection;
            break;
        }
    }
    assert(i < server->numslots);
    
    connection->request = NULL;
    connection->request_last = NULL;

    do_log(D_SERVER_CONN, "C... %s:%d.\n",
           connection->server->name, connection->server->port);
    httpSetTimeout(connection, 60);
    connection->connecting = CONNECTING_DNS;
    do_gethostbyname(server->name, 0,
                     httpServerConnectionDnsHandler,
                     connection);
    return 1;
}

int
httpServerConnectionDnsHandler(int status, GethostbynameRequestPtr request)
{
    HTTPConnectionPtr connection = request->data;

    httpSetTimeout(connection, -1);

    if(status <= 0) {
        AtomPtr message;
        message = internAtomF("Host %s lookup failed: %s",
                              request->name ?
                              request->name->string : "(unknown)",
                              request->error_message ?
                              request->error_message->string :
                              pstrerror(-status));
        do_log(L_ERROR, "Host %s lookup failed: %s (%d).\n", 
               request->name ?
               request->name->string : "(unknown)",
               request->error_message ?
               request->error_message->string :
               pstrerror(-status), -status);
        connection->connecting = 0;
        if(connection->server->request)
            httpServerAbortRequest(connection->server->request, 1, 504,
                                   retainAtom(message));
        httpServerAbort(connection, 1, 502, message);
        return 1;
    }

    if(request->addr->string[0] == DNS_CNAME) {
        if(request->count > 10) {
            AtomPtr message = internAtom("DNS CNAME loop");
            do_log(L_ERROR, "DNS CNAME loop.\n");
            connection->connecting = 0;
            if(connection->server->request)
                httpServerAbortRequest(connection->server->request, 1, 504,
                                       retainAtom(message));
            httpServerAbort(connection, 1, 504, message);
            return 1;
        }
            
        httpSetTimeout(connection, 60);
        do_gethostbyname(request->addr->string + 1, request->count + 1,
                         httpServerConnectionDnsHandler,
                         connection);
        return 1;
    }

    connection->connecting = CONNECTING_CONNECT;
    httpSetTimeout(connection, 60);
    do_connect(retainAtom(request->addr), connection->server->addrindex,
               connection->server->port,
               httpServerConnectionHandler, connection);
    return 1;
}

int
httpServerConnectionHandler(int status,
                            FdEventHandlerPtr event,
                            ConnectRequestPtr request)
{
    HTTPConnectionPtr connection = request->data;
    int rc;

    assert(connection->fd < 0);
    if(request->fd >= 0) {
        connection->fd = request->fd;
        connection->server->addrindex = request->index;
    }
    httpSetTimeout(connection, -1);

    if(status < 0) {
        AtomPtr message = 
            internAtomError(-status, "Connect to %s:%d failed",
                            connection->server->name,
                            connection->server->port);
        do_log_error(L_ERROR, -status, "Connect to %s:%d failed",
                     connection->server->name, connection->server->port);
        connection->connecting = 0;
        if(connection->server->request)
            httpServerAbortRequest(connection->server->request,
                                   status != -ECLIENTRESET, 504, 
                                   retainAtom(message));
        httpServerAbort(connection, status != -ECLIENTRESET, 504, message);
        return 1;
    }

    do_log(D_SERVER_CONN, "C    %s:%d.\n",
           connection->server->name, connection->server->port);

    rc = setNodelay(connection->fd, 1);
    if(rc < 0)
        do_log_error(L_WARN, errno, "Couldn't disable Nagle's algorithm");

    connection->connecting = 0;
    httpServerTrigger(connection->server);
    return 1;
}

int
httpServerIdleHandler(int a, FdEventHandlerPtr event)
{
    HTTPConnectionPtr connection = *(HTTPConnectionPtr*)event->data;
    HTTPServerPtr server = connection->server;
    int i;

    assert(!connection->request);

    do_log(D_SERVER_CONN, "Idle connection to %s:%d died.\n", 
           connection->server->name, connection->server->port);

    for(i = 0; i < server->maxslots; i++) {
        if(connection == server->connection[i]) {
            server->idleHandler[i] = NULL;
            break;
        }
    }
    assert(i < server->maxslots);

    httpServerAbort(connection, 1, 504, internAtom("Timeout"));
    return 1;
}

/* Discard aborted requests at the head of the queue. */
static void
httpServerDiscardRequests(HTTPServerPtr server)
{
    HTTPRequestPtr request;
    while(server->request && !server->request->request) {
        request = server->request;
        server->request = request->next;
        request->next = NULL;
        if(server->request == NULL)
            server->request_last = NULL;
        request->object->flags &= ~(OBJECT_INPROGRESS | OBJECT_VALIDATING);
        releaseNotifyObject(request->object);
        request->object = NULL;
        httpDestroyRequest(request);
    }
}

static int
pipelineIsSmall(HTTPConnectionPtr connection)
{
    HTTPRequestPtr request = connection->request;

    if(pipelineAdditionalRequests <= 0)
        return 0;
    else if(pipelineAdditionalRequests >= 2)
        return 1;

    if(!request)
        return 1;
    if(request->next || !request->persistent)
        return 0;
    if(request->method == METHOD_HEAD || 
       request->method == METHOD_CONDITIONAL_GET)
        return 1;
    if(request->to >= 0 && connection->server->rate > 0 &&
       request->to - request->from < connection->server->rate * 
       smallRequestTime)
        return 1;
    return 0;
}

static int
numRequests(HTTPServerPtr server)
{
    int n = 0;
    HTTPRequestPtr request = server->request;
    while(request) {
        n++;
        request = request->next;
    }
    return n;
}

HTTPConnectionPtr
httpServerGetConnection(HTTPServerPtr server)
{
    int i;
    int connecting = 0, empty = 0;

    /* Try to find an idle connection */
    for(i = 0; i < server->numslots; i++) {
        if(server->connection[i]) {
            if(!server->connection[i]->connecting) {
                if(!server->connection[i]->request) {
                    if(server->idleHandler[i])
                        unregisterFdEvent(server->idleHandler[i]);
                    server->idleHandler[i] = NULL;
                    return server->connection[i];
                }
            } else
                connecting++;
        } else
            empty++;
    }

    /* If there's an empty slot, schedule connection creation */
    if(empty) {
        /* Don't open a connection if there are already enough in
           progress, except if the server doesn't do persistent
           connections and there's only one in progress. */
        if((connecting == 0 || (server->persistent <= 0 && connecting <= 1)) ||
           connecting < numRequests(server)) {
            httpServerConnection(server);
        }
    }

    /* Find a connection that can accept additional requests */
    if(server->version == HTTP_11 && server->pipeline >= 4) {
        for(i = 0; i < serverSlots; i++) {
            if(server->connection[i] && !server->connection[i]->connecting &&
               pipelineIsSmall(server->connection[i])) {
                if(server->idleHandler[i])
                    unregisterFdEvent(server->idleHandler[i]);
                server->idleHandler[i] = NULL;
                return server->connection[i];
            }
        }
    }
    return NULL;
}

int
httpServerTrigger(HTTPServerPtr server)
{
    HTTPConnectionPtr connection;
    HTTPRequestPtr request;
    int idle, n, i, rc;

    while(server->request) {
        httpServerDiscardRequests(server);

        if(!server->request)
            break;

        if(REQUEST_SIDE(server->request)) {
            rc = httpServerSideRequest(server);
            /* If rc is 0, httpServerSideRequest didn't dequeue this
               request.  Go through the scheduling loop again, come
               back later. */
            if(rc <= 0) break;
            continue;
        }
        connection = httpServerGetConnection(server);
        if(!connection) break;

        /* If server->pipeline <= 0, we don't do pipelining.  If
           server->pipeline is 1, then we are ready to start probing
           for pipelining on the server; we then send exactly two
           requests in what is hopefully a single packet to check
           whether the server has the nasty habit of discarding its
           input buffers after each request.
           If server->pipeline is 2 or 3, the pipelining probe is in
           progress on this server, and we don't pipeline anything
           until it succeeds.  When server->pipeline >= 4, pipelining
           is believed to work on this server. */
        if(server->version != HTTP_11 || server->pipeline <= 0 ||
           server->pipeline == 2 || server->pipeline == 3) {
            if(connection->pipelined == 0)
                n = 1;
            else
                n = 0;
        } else if(server->pipeline == 1) {
            if(connection->pipelined == 0)
                n = MIN(2, maxPipelineTrain);
            else
                n = 0;
        } else {
            n = maxPipelineTrain;
        }
    
        idle = !connection->pipelined;
        i = 0;
        while(server->request && connection->pipelined < n) {
            httpServerDiscardRequests(server);
            if(!server->request) break;
            request = server->request;
            assert(request->request->request == request);
            rc = httpWriteRequest(connection, request, -1);
            if(rc < 0) {
                if(i == 0)
                    httpServerAbortRequest(request, rc != -ECLIENTRESET, 503,
                                           internAtom("Couldn't "
                                                      "write request"));
                break;
            }
            do_log(D_SERVER_CONN, "W: ");
            do_log_n(D_SERVER_CONN, 
                     request->object->key, request->object->key_size);
            do_log(D_SERVER_CONN, " (%d)\n", request->method);
            if(connection->pipelined == 0)
                request->time0 = current_time;
            i++;
            server->request = request->next;
            request->next = NULL;
            if(server->request == NULL)
                server->request_last = NULL;
            httpQueueRequest(connection, request);
            connection->pipelined++;
        }
        if(server->persistent > 0 && server->pipeline == 1 && i >= 2)
            server->pipeline = 2;

        if(i > 0) httpServerSendRequest(connection);

        if(idle && connection->pipelined > 0)
            httpServerReply(connection, 0);

        if(i == 0) break;
    }

    for(i = 0; i < server->maxslots; i++) {
        if(server->connection[i] &&
           !server->connection[i]->connecting &&
           !server->connection[i]->request) {
            /* Artificially age any fresh connections that aren't used
               straight away; this is necessary for the logic for POST and 
               the logic that determines whether a given request should be 
               restarted. */
            if(server->connection[i]->serviced == 0)
                server->connection[i]->serviced = 1;
            if(!server->idleHandler[i])
                server->idleHandler[i] = 
                    registerFdEvent(server->connection[i]->fd, POLLIN,
                                    httpServerIdleHandler,
                                    sizeof(HTTPConnectionPtr),
                                    &server->connection[i]);
            if(!server->idleHandler[i]) {
                do_log(L_ERROR, "Couldn't register idle handler.\n");
                httpServerFinish(server->connection[i], 1, 0);
            }
        }
    }

    return 1;
}

int
httpServerSideRequest(HTTPServerPtr server)
{
    HTTPRequestPtr request = server->request;
    HTTPConnectionPtr connection;
    HTTPRequestPtr requestor = request->request;
    HTTPConnectionPtr client = requestor->connection;
    int rc, i, freeslots, idle, connecting;

    assert(REQUEST_SIDE(request));

    connection = NULL;
    freeslots = 0;
    idle = -1;
    connecting = 0;

    /* Find a fresh connection */
    for(i = 0; i < server->numslots; i++) {
        if(!server->connection[i])
            freeslots++;
        else if(!server->connection[i]->connecting) {
            if(!server->connection[i]->request) {
                if(server->connection[i]->serviced == 0) {
                    if(server->idleHandler[i])
                        unregisterFdEvent(server->idleHandler[i]);
                    server->idleHandler[i] = NULL;
                    connection = server->connection[i];
                    break;
                } else {
                    idle = i;
                }
            }
        } else {
            connecting++;
        }
    }

    if(!connection) {
        /* Make sure that a fresh connection will be established at some
           point, then wait until httpServerTrigger calls us again. */
        if(freeslots) {
            httpServerConnection(server);
        } else {
            if(idle >= 0) {
                /* Shutdown a random idle connection */
                pokeFdEvent(server->connection[idle]->fd, 
                            -EDOSHUTDOWN, POLLIN | POLLOUT);
            }
        }
        return 0;
    }

    rc = httpWriteRequest(connection, request, client->bodylen);
    if(rc < 0) {
        do_log(L_ERROR, "Couldn't write POST or PUT request.\n");
        httpServerAbortRequest(request, rc != -ECLIENTRESET, 503, 
                               internAtom("Couldn't write request"));
        return 0;
    }
    server->request = request->next;
    request->next = NULL;
    if(server->request == NULL)
        server->request_last = NULL;
    httpQueueRequest(connection, request);
    connection->pipelined = 1;
    request->time0 = current_time;
    connection->reqoffset = 0;
    connection->bodylen = client->bodylen;
    httpServerDoSide(connection);
    return 1;
}

int 
httpServerDoSide(HTTPConnectionPtr connection)
{
    HTTPRequestPtr request = connection->request;
    HTTPRequestPtr requestor = request->request;
    HTTPConnectionPtr client = requestor->connection;
    int len = MIN(client->reqlen - client->reqbegin,
                  connection->bodylen - connection->reqoffset);
    int doflush = 
        len > 0 &&
        (len >= 1500 ||
         client->reqbegin > 0 ||
         (connection->reqoffset + client->reqlen - client->reqbegin) >=
         connection->bodylen);
    int done = connection->reqoffset >= connection->bodylen;

    assert(connection->bodylen >= 0);

    httpSetTimeout(connection, 60);

    if(connection->reqlen > 0) {
        /* Send the headers, but don't send any part of the body if
           we're in wait_continue. */
        do_stream_2(IO_WRITE,
                    connection->fd, 0,
                    connection->reqbuf, connection->reqlen,
                    client->reqbuf + client->reqbegin, 
                    request->wait_continue ? 0 : len,
                    httpServerSideHandler2, connection);
        httpServerReply(connection, 0);
    } else if(request->object->flags & OBJECT_ABORTED) {
        if(connection->reqbuf)
            dispose_chunk(connection->reqbuf);
        connection->reqbuf = NULL;
        connection->reqlen = 0;
        pokeFdEvent(connection->fd, -ESHUTDOWN, POLLIN);
        client->flags |= CONN_SIDE_READER;
        do_stream(IO_READ | IO_IMMEDIATE,
                  client->fd, 0, NULL, 0,
                  httpClientSideHandler, client);
    } else if(!request->wait_continue && doflush) {
        /* Make sure there's a reqbuf, as httpServerFinish uses
           it to determine if there's a writer. */
        if(connection->reqbuf == NULL)
            connection->reqbuf = get_chunk();
        assert(connection->reqbuf != NULL);
        do_stream(IO_WRITE,
                  connection->fd, 0,
                  client->reqbuf + client->reqbegin, len,
                  httpServerSideHandler, connection);
    } else {
        if(connection->reqbuf)
            dispose_chunk(connection->reqbuf);
        connection->reqbuf = NULL;
        connection->reqlen = 0;
        if(request->wait_continue) {
            ObjectHandlerPtr ohandler;
            do_log(D_SERVER_CONN, "W... %s:%d.\n",
                   connection->server->name, connection->server->port);
            ohandler = 
                registerObjectHandler(request->object,
                                      httpServerContinueObjectHandler,
                                      sizeof(connection), &connection);
            if(ohandler)
                return 1;
            else
                do_log(L_ERROR, "Couldn't register object handler.\n");
            /* Fall through -- the client side will clean up. */
        }
        client->flags |= CONN_SIDE_READER;
        do_stream(IO_READ | (done ? IO_IMMEDIATE : 0 ) | IO_NOTNOW,
                  client->fd, client->reqlen,
                  client->reqbuf, CHUNK_SIZE,
                  httpClientSideHandler, client);
    }
    return 1;
}

static int
httpClientDelayedDoSideHandler(TimeEventHandlerPtr event)
{
    HTTPConnectionPtr connection = *(HTTPConnectionPtr*)event->data;
    httpServerDoSide(connection);
    return 1;
}

static int
httpServerDelayedDoSide(HTTPConnectionPtr connection)
{
    TimeEventHandlerPtr handler;
    handler = scheduleTimeEvent(1, httpClientDelayedDoSideHandler,
                                sizeof(connection), &connection);
    if(!handler) {
        do_log(L_ERROR, "Couldn't schedule DoSide -- freeing memory.\n");
        free_chunk_arenas();
        handler = scheduleTimeEvent(1, httpClientDelayedDoSideHandler,
                                    sizeof(connection), &connection);
        do_log(L_ERROR, "Couldn't schedule DoSide.\n");
        /* Somebody will hopefully end up timing out. */
        return 1;
    }
    return 1;
}

static int
httpServerSideHandlerCommon(int kind, int status,
                            FdEventHandlerPtr event,
                            StreamRequestPtr srequest)
{
    HTTPConnectionPtr connection = srequest->data;
    HTTPRequestPtr request = connection->request;
    HTTPRequestPtr requestor = request->request;
    HTTPConnectionPtr client = requestor->connection;
    int bodylen;

    assert(request->object->flags & OBJECT_INPROGRESS);

    if(status) {
        do_log_error(L_ERROR, -status, "Couldn't write to server");
        dispose_chunk(connection->reqbuf);
        connection->reqbuf = NULL;
        if(status != -ECLIENTRESET)
            shutdown(connection->fd, 2);
        abortObject(request->object, 503,
                    internAtom("Couldn't write to server"));
        /* Let the read side handle the error */
        httpServerDoSide(connection);
        return 1;
    }

    assert(srequest->offset > 0);

    if(kind == 2) {
        if(srequest->offset < connection->reqlen)
            return 0;
        bodylen = srequest->offset - connection->reqlen;
        dispose_chunk(connection->reqbuf);
        connection->reqbuf = NULL;
        connection->reqlen = 0;
    } else {
        bodylen = srequest->offset;
    }


    assert(client->reqbegin + bodylen <= client->reqlen);

    if(client->reqlen > client->reqbegin + bodylen)
        memmove(client->reqbuf, client->reqbuf + client->reqbegin + bodylen,
                client->reqlen - client->reqbegin - bodylen);
    client->reqlen -= bodylen + client->reqbegin;
    client->reqbegin = 0;
    connection->reqoffset += bodylen;
    httpServerDoSide(connection);
    return 1;
}

int
httpServerSideHandler(int status,
                      FdEventHandlerPtr event,
                      StreamRequestPtr srequest)
{
    return httpServerSideHandlerCommon(1, status, event, srequest);
}

int
httpServerSideHandler2(int status,
                       FdEventHandlerPtr event,
                       StreamRequestPtr srequest)
{
    return httpServerSideHandlerCommon(2, status, event, srequest);
}

static int
httpServerContinueObjectHandler(int status, ObjectHandlerPtr ohandler)
{
    ObjectPtr object = ohandler->object;
    HTTPConnectionPtr connection = *(HTTPConnectionPtr*)ohandler->data;

    assert(object == connection->request->object);
    if(connection->request->wait_continue)
        return 0;
    httpServerDelayedDoSide(connection);
    return 1;
}

/* s is 0 to keep the connection alive, 1 to shutdown the connection,
   and -1 to keep the connection alive and keep the current request. */
void
httpServerFinish(HTTPConnectionPtr connection, int s, int offset)
{
    HTTPServerPtr server = connection->server;
    HTTPRequestPtr request = connection->request;
    int i;

    if(request) {
        assert(connection->pipelined >= 1);
        assert((connection->pipelined > 1) == (request->next != NULL));
    } else {
        assert(connection->pipelined == 0);
    }

    if(s == 0 && (!connection->request || !connection->request->persistent))
        s = 1;

    if(connection->reqbuf) {
        /* As most normal requests go out in a single packet, this is
           extremely unlikely to happen.  As for POST/PUT requests,
           they are not pipelined, so this can only happen if the
           server sent an error reply early. */
        assert(connection->fd >= 0);
        shutdown(connection->fd, 1);
        pokeFdEvent(connection->fd, -EDOSHUTDOWN, POLLOUT);
        httpServerDelayedFinish(connection);
        goto done;
    }

    if(s >= 0 && request) {
        /* Update statistics about the server */
        int size = -1, d = -1, rtt = -1, rate = -1;
        if(connection->offset > 0 && request->from >= 0)
            size = connection->offset - request->from;
        if(request->time0.tv_sec > 0 && request->time1.tv_sec > 0) {
            d = timeval_minus_usec(&current_time, &request->time1);
            rtt = timeval_minus_usec(&request->time1, &request->time0);
            if(size >= 4096 && d > 50000)
                rate = ((double)size / (double)d) * 1000000.0 + 0.5;
        }
        request->time0 = null_time;
        request->time1 = null_time;

        if(rtt > 0) {
            if(server->rtt > 0)
                server->rtt = (3 * server->rtt + rtt + 2) / 4;
            else
                server->rtt = rtt;
        }
        if(rate > 0) {
            if(server->rate > 0)
                server->rate = (3 * server->rate + rate + 2) / 4;
            else
                server->rate = rate;
        }

        httpDequeueRequest(connection);
        connection->pipelined--;
        request->object->flags &= ~(OBJECT_INPROGRESS | OBJECT_VALIDATING);
        if(request->request) {
            request->request->request = NULL;
            request->request = NULL;
        }
        releaseNotifyObject(request->object);
        request->object = NULL;
        httpDestroyRequest(request);
    }

    do_log(D_SERVER_CONN, "Done with server %s:%d connection (%d)\n",
           connection->server->name, connection->server->port, s);

    assert(offset <= connection->len);

    if(s <= 0) {
        if(offset < connection->len) {
            assert(connection->buf != NULL);
            if(!connection->pipelined) {
                do_log(L_WARN, 
                       "Closing connection to %s:%d: "
                       "%d stray bytes of data.\n",
                       server->name, server->port, connection->len - offset);
                s = 1;
            } else {
                memmove(connection->buf, connection->buf + offset,
                        connection->len - offset);
                connection->len = connection->len - offset;
            }
        } else {
            connection->len = 0;
        }
    }

    connection->server->time = current_time.tv_sec;
    connection->serviced++;

    if(s > 0) {
        if(connection->timeout)
            cancelTimeEvent(connection->timeout);
        connection->timeout = NULL;
        if(connection->buf) {
            dispose_chunk(connection->buf);
            connection->buf = NULL;
        }
        if(connection->fd >= 0)
            close(connection->fd);
        connection->fd = -1;
        server->persistent -= 1;
        if(server->persistent < -5)
            server->numslots = MIN(server->maxslots, serverMaxSlots);
        if(connection->request) {
            HTTPRequestPtr req;
            do_log(D_SERVER_CONN, "Restarting pipeline to %s:%d.\n",
                   server->name, server->port);
            if(server->pipeline == 2)
                server->pipeline -= 20;
            else
                server->pipeline -= 5;
            req = connection->request;
            while(req) {
                req->connection = NULL;
                req = req->next;
            }
            if(server->request)
                connection->request_last->next = server->request;
            else
                server->request_last = connection->request_last;
            server->request = connection->request;
            connection->request = NULL;
            connection->request_last = NULL;
        }
        /* Make sure we don't get confused into thinking a probe
           is in progress. */
        if(server->pipeline == 2 || server->pipeline == 3)
            server->pipeline = 1;
        for(i = 0; i < server->maxslots; i++)
            if(connection == server->connection[i])
                break;
        assert(i < server->maxslots);
        if(server->idleHandler[i])
            unregisterFdEvent(server->idleHandler[i]);
        server->idleHandler[i] = NULL;
        server->connection[i] = NULL;
        free(connection);
    } else {
        server->persistent += 1;
        if(server->persistent > 0)
            server->numslots = MIN(server->maxslots, serverSlots);
        httpSetTimeout(connection, 60);
        /* See httpServerTrigger */
        if(connection->pipelined ||
           (server->version == HTTP_11 && server->pipeline <= 0) ||
           (server->pipeline == 3)) {
            server->pipeline++;
        }
        if(s < 0 || connection->pipelined) {
            httpServerReply(connection, 1);
        } else {
            if(connection->buf) {
                dispose_chunk(connection->buf);
                connection->buf = NULL;
            }
        }
    }

 done:
    httpServerTrigger(server);
}

static int
httpServerDelayedFinishHandler(TimeEventHandlerPtr event)
{
    HTTPConnectionPtr connection = *(HTTPConnectionPtr*)event->data;
    httpServerFinish(connection, 1, 0);
    return 1;
}

static void
httpServerDelayedFinish(HTTPConnectionPtr connection)
{
    TimeEventHandlerPtr handler;

    handler = scheduleTimeEvent(1, httpServerDelayedFinishHandler,
                                sizeof(connection), &connection);
    if(!handler) {
        do_log(L_ERROR,
               "Couldn't schedule delayed finish -- freeing memory.");
        free_chunk_arenas();
        handler = scheduleTimeEvent(1, httpServerDelayedFinishHandler,
                                    sizeof(connection), &connection);
        if(!handler) {
            do_log(L_ERROR,
                   "Couldn't schedule delayed finish -- aborting.\n");
            polipoExit();
        }
    }
}

void
httpServerReply(HTTPConnectionPtr connection, int immediate)
{
    assert(connection->pipelined > 0);

    if(connection->request->request == NULL) {
        do_log(L_WARN, "Aborting pipeline on %s:%d.\n",
               connection->server->name, connection->server->port);
        httpServerFinish(connection, 1, 0);
        return;
    }

    do_log(D_SERVER_CONN, "R: ");
    do_log_n(D_SERVER_CONN, connection->request->object->key,
             connection->request->object->key_size);
    do_log(D_SERVER_CONN, " (%d)\n", connection->request->method);

    if(connection->len == 0) {
        if(connection->buf) {
            dispose_chunk(connection->buf);
            connection->buf = NULL;
        }
    }

    httpSetTimeout(connection, 60);
    do_stream_buf(IO_READ | (immediate ? IO_IMMEDIATE : 0) | IO_NOTNOW,
                  connection->fd, connection->len,
                  &connection->buf, CHUNK_SIZE,
                  httpServerReplyHandler, connection);
}

int
httpConnectionPipelined(HTTPConnectionPtr connection)
{
    HTTPRequestPtr request = connection->request;
    int i = 0;
    while(request) {
        i++;
        request = request->next;
    }
    return i;
}

void
httpServerUnpipeline(HTTPRequestPtr request)
{
    HTTPConnectionPtr connection = request->connection;
    HTTPServerPtr server = connection->server;

    request->persistent = 0;
    if(request->next) {
        HTTPRequestPtr req;
        do_log(L_WARN,
               "Restarting pipeline to %s:%d.\n", 
               connection->server->name, connection->server->port);
        req = request->next;
        while(req) {
            req->connection = NULL;
            req = req->next;
        }
        if(server->request)
            connection->request_last->next = server->request;
        else
            server->request_last = connection->request_last;
        server->request = request->next;
        request->next = NULL;
        connection->request_last = request;
    }
    connection->pipelined = httpConnectionPipelined(connection);
}

void
httpServerRestart(HTTPConnectionPtr connection)
{
    HTTPServerPtr server = connection->server;
    HTTPRequestPtr request = connection->request;

    if(request) {
        HTTPRequestPtr req;
        if(request->next)
            do_log(L_WARN,
                   "Restarting pipeline to %s:%d.\n", 
                   connection->server->name, connection->server->port);
        req = request;
        while(req) {
            req->connection = NULL;
            req = req->next;
        }
        if(server->request)
            connection->request_last->next = server->request;
        else
            server->request_last = connection->request_last;
        server->request = request;
        connection->request = NULL;
        connection->request_last = NULL;
    }
    connection->pipelined = 0;
    httpServerFinish(connection, 1, 0);
}

int
httpServerRequest(ObjectPtr object, int method, int from, int to, 
                  HTTPRequestPtr requestor, void *closure)
{
    int rc;
    char name[132];
    int port;
    int x, y, z;

    assert(from >= 0 && (to < 0 || to > from));
    assert(closure == NULL);
    assert(!(object->flags & OBJECT_LOCAL));
    assert(object->type == OBJECT_HTTP);

    if(object->flags & OBJECT_INPROGRESS)
        return 1;

    assert(requestor->request == NULL);

    if(requestor->requested)
        return 0;

    if(proxyOffline)
        return -1;

    if(urlForbidden(object->key, object->key_size)) {
        do_log(L_FORBIDDEN, "Forbidden URL ");
        do_log_n(L_FORBIDDEN, object->key, object->key_size);
        do_log(L_FORBIDDEN, "\n");
        abortObject(object, 403, internAtom("Forbidden URL"));
        notifyObject(object);
        if(REQUEST_SIDE(requestor)) {
            HTTPConnectionPtr client = requestor->connection;
            client->flags |= CONN_SIDE_READER;
            do_stream(IO_READ | IO_IMMEDIATE | IO_NOTNOW,
                      client->fd, client->reqlen,
                      client->reqbuf, CHUNK_SIZE,
                      httpClientSideHandler, client);
        }
        return 1;
    }

    rc = parseUrl(object->key, object->key_size, &x, &y, &port, &z);
    
    if(rc < 0 || x < 0 || y < 0 || y - x > 131) {
        do_log(L_ERROR, "Couldn't parse URL: ");
        do_log_n(L_ERROR, object->key, object->key_size);
        do_log(L_ERROR, "\n");
        abortObject(object, 400, internAtom("Couldn't parse URL"));
        notifyObject(object);
        return 1;
    }

    if(!intListMember(port, allowedPorts)) {
        do_log(L_ERROR, "Attempted connection to port %d.\n", port);
        abortObject(object, 403, internAtom("Forbidden port"));
        notifyObject(object);
        return 1;
    }

    memcpy(name, ((char*)object->key) + x, y - x);
    name[y - x] = '\0';

    requestor->requested = 1;
    rc = httpMakeServerRequest(name, port, object, method, from, to,
                               requestor);
                                   
    if(rc < 0) {
        abortObject(object, 
                    503, internAtom("Couldn't schedule server request"));
        notifyObject(object);
        return 1;
    }

    return 1;
}

int
httpWriteRequest(HTTPConnectionPtr connection, HTTPRequestPtr request,
                 int bodylen)
{
    ObjectPtr object = request->object;
    int from = request->from, to = request->to, method = request->method;
    char *url = object->key, *m;
    int url_size = object->key_size;
    int x, y, port, z, location_size;
    char *location;
    int rc;
    int l, n;

    assert(method != METHOD_NONE);

    if(request->method == METHOD_GET || 
       request->method == METHOD_CONDITIONAL_GET) {
        if(to >= 0) {
            assert(to >= from);
            if(to == from) {
                do_log(L_ERROR, "Requesting empty segment?\n");
                return -1;
            }
        }

        if(object->flags & OBJECT_DYNAMIC) {
            from = 0;
            to = -1;
        } else {
            objectFillFromDisk(object, from / CHUNK_SIZE * CHUNK_SIZE, 1);
            l = objectHoleSize(request->object, from);
            if(l > 0) {
                if(to <= 0 || to > from + l)
                    to = from + l;
            }

            if(pmmSize && connection->server->pipeline >= 4) {
                if(from == 0)
                    to = to < 0 ? pmmFirstSize : MIN(to, pmmFirstSize);
                else
                    to = to < 0 ? from + pmmSize : MIN(to, from + pmmSize);
            }

            if(from % CHUNK_SIZE != 0)
                if(objectHoleSize(object, from - 1) != 0)
                    from = from / CHUNK_SIZE * CHUNK_SIZE;
        }
    }

    rc = parseUrl(url, url_size, &x, &y, &port, &z);

    if(rc < 0 || x < 0 || y < 0) {
        return -1;
    }

    if(connection->reqbuf == NULL) {
        connection->reqbuf = get_chunk();
        if(connection->reqbuf == NULL)
            return -1;
        connection->reqlen = 0;
    }

    if(method == METHOD_CONDITIONAL_GET &&
       object->last_modified < 0 && object->etag == NULL)
        method = request->method = METHOD_GET;

    n = connection->reqlen;
    switch(method) {
    case METHOD_GET:
    case METHOD_CONDITIONAL_GET: m = "GET"; break;
    case METHOD_HEAD: m = "HEAD"; break;
    case METHOD_POST: m = "POST"; break;
    case METHOD_PUT: m = "PUT"; break;
    default: abort();
    }
    n = snnprintf(connection->reqbuf, n, CHUNK_SIZE, "%s ", m);

    if(connection->server->isProxy) {
        n = snnprint_n(connection->reqbuf, n, CHUNK_SIZE,
                       url, url_size);
    } else {
        if(url_size - z == 0) {
            location = "/";
            location_size = 1;
        } else {
            location = url + z;
            location_size = url_size - z;
        }
        
        n = snnprint_n(connection->reqbuf, n, CHUNK_SIZE, 
                       location, location_size);
    }
    
    do_log(D_SERVER_REQ, "Server request: ");
    do_log_n(D_SERVER_REQ, url + x, y - x);
    do_log(D_SERVER_REQ, ": ");
    do_log_n(D_SERVER_REQ, connection->reqbuf, n);
    do_log(D_SERVER_REQ, " (method %d from %d to %d, 0x%lx for 0x%lx)\n",
           method, from, to,
           (unsigned long)connection, (unsigned long)object);

    n = snnprintf(connection->reqbuf, n, CHUNK_SIZE, " HTTP/1.1");

    n = snnprintf(connection->reqbuf, n, CHUNK_SIZE, "\r\nHost: ");
    n = snnprint_n(connection->reqbuf, n, CHUNK_SIZE, url + x, y - x);
    if(port != 80) {
        n = snnprintf(connection->reqbuf, n, CHUNK_SIZE,
                      ":%d", port);
    }

    if(connection->server->isProxy && parentAuthCredentials) {
        n = buildServerAuthHeaders(connection->reqbuf, n, CHUNK_SIZE,
                                   parentAuthCredentials);
    }

    if(bodylen >= 0)
        n = snnprintf(connection->reqbuf, n, CHUNK_SIZE,
                      "\r\nContent-Length: %d", bodylen);

    if(request->wait_continue)
        n = snnprintf(connection->reqbuf, n, CHUNK_SIZE,
                      "\r\nExpect: 100-continue");

    if(method != METHOD_HEAD && (from > 0 || to >= 0)) {
        if(to >= 0) {
            n = snnprintf(connection->reqbuf, n, CHUNK_SIZE,
                          "\r\nRange: bytes=%d-%d", from, to - 1);
        } else {
            n = snnprintf(connection->reqbuf, n, CHUNK_SIZE,
                          "\r\nRange: bytes=%d-", from);
        }
    }

    if(method == METHOD_GET && object->etag && (from > 0 || to >= 0)) {
        if(request->request && request->request->request == request &&
           request->request->from == 0 && request->request->to == -1 &&
           pmmSize == 0 && pmmFirstSize == 0)
            n = snnprintf(connection->reqbuf, n, CHUNK_SIZE,
                          "\r\nIf-Range: \"%s\"", object->etag);
    }

    if(method == METHOD_CONDITIONAL_GET) {
        if(object->last_modified >= 0) {
            n = snnprintf(connection->reqbuf, n, CHUNK_SIZE,
                          "\r\nIf-Modified-Since: ");
            n = format_time(connection->reqbuf, n, CHUNK_SIZE,
                            object->last_modified);
        }
        if(object->etag) {
            n = snnprintf(connection->reqbuf, n, CHUNK_SIZE,
                          "\r\nIf-None-Match: \"%s\"", object->etag);
        }
    }

    n = httpPrintCacheControl(connection->reqbuf, n, CHUNK_SIZE,
                              0, &request->cache_control);
    if(n < 0)
        return -1;

    if(request->request && request->request->headers) {
        n = snnprint_n(connection->reqbuf, n, CHUNK_SIZE,
                       request->request->headers->string, 
                       request->request->headers->length);
    }
    if(request->request && request->request->via) {
        n = snnprintf(connection->reqbuf, n, CHUNK_SIZE,
                      "\r\nVia: %s, 1.1 %s",
                      request->request->via->string, proxyName->string);
    } else {
        n = snnprintf(connection->reqbuf, n, CHUNK_SIZE,
                      "\r\nVia: 1.1 %s",
                      proxyName->string);
    }

    n = snnprintf(connection->reqbuf, n, CHUNK_SIZE,
                  "\r\nConnection: %s\r\n\r\n",
                  request->persistent?"keep-alive":"close");
    if(n < 0 || n >= CHUNK_SIZE - 1)
        return -1;
    connection->reqlen = n;
    return n;
}

int
httpServerHandler(int status, 
                  FdEventHandlerPtr event,
                  StreamRequestPtr srequest)
{
    HTTPConnectionPtr connection = srequest->data;
    AtomPtr message;
    
    assert(connection->request->object->flags & OBJECT_INPROGRESS);

    if(connection->reqlen == 0) {
        do_log(D_SERVER_REQ, "Writing aborted on 0x%lx\n", 
               (unsigned long)connection);
        goto fail;
    }

    if(status == 0 && !streamRequestDone(srequest)) {
        httpSetTimeout(connection, 60);
        return 0;
    }
    
    dispose_chunk(connection->reqbuf);
    connection->reqbuf = NULL;

    if(status) {
        if(connection->serviced >= 1) {
            httpServerRestart(connection);
            return 1;
        }
        if(status >= 0 || status == ECONNRESET) {
            message = internAtom("Couldn't send request to server: "
                                 "short write");
        } else {
            if(status != -EPIPE)
                do_log_error(L_ERROR, -status,
                             "Couldn't send request to server");
            message = 
                internAtomError(-status, "Couldn't send request to server");
        }
        goto fail;
    }
    
    return 1;

 fail:
    dispose_chunk(connection->reqbuf);
    connection->reqbuf = NULL;
    shutdown(connection->fd, 2);
    pokeFdEvent(connection->fd, -EDOSHUTDOWN, POLLIN);
    httpSetTimeout(connection, 60);
    return 1;
}

int
httpServerSendRequest(HTTPConnectionPtr connection)
{
    assert(connection->server);

    if(connection->reqlen == 0) {
        do_log(D_SERVER_REQ, 
               "Writing aborted on 0x%lx\n", (unsigned long)connection);
        dispose_chunk(connection->reqbuf);
        connection->reqbuf = NULL;
        shutdown(connection->fd, 2);
        pokeFdEvent(connection->fd, -EDOSHUTDOWN, POLLIN | POLLOUT);
        return -1;
    }

    httpSetTimeout(connection, 60);
    do_stream(IO_WRITE, connection->fd, 0,
              connection->reqbuf, connection->reqlen,
              httpServerHandler, connection);
    return 1;
}

int
httpServerReplyHandler(int status,
                       FdEventHandlerPtr event, 
                       StreamRequestPtr srequest)
{
    HTTPConnectionPtr connection = srequest->data;
    HTTPRequestPtr request = connection->request;
    int i, body;

    assert(request->object->flags & OBJECT_INPROGRESS);
    if(status < 0) {
        if(connection->serviced >= 1) {
            httpServerRestart(connection);
            return 1;
        }
        if(status != -ECLIENTRESET)
            do_log_error(L_ERROR, -status, "Read from server failed");
        httpServerAbort(connection, status != -ECLIENTRESET, 502, 
                        internAtomError(-status, "Read from server failed"));
        return 1;
    }

    i = findEndOfHeaders(connection->buf, 0, srequest->offset, &body);
    connection->len = srequest->offset;

    if(i >= 0) {
        request->time1 = current_time;
        return httpServerHandlerHeaders(status, event, srequest, connection);
    }

    if(connection->len >= CHUNK_SIZE) {
        do_log(L_ERROR, "Couldn't find end of server's headers\n");
        httpServerAbort(connection, 1, 502,
                        internAtom("Couldn't find end of server's headers"));
        return 1;
    }

    if(status) {
        if(connection->serviced >= 1) {
            httpServerRestart(connection);
            return 1;
        }
        if(status < 0) {
            do_log(L_ERROR, 
                   "Error reading server headers: %d\n", -status);
            httpServerAbort(connection, status != -ECLIENTRESET, 502, 
                            internAtomError(-status, 
                                            "Error reading server headers"));
        } else
            httpServerAbort(connection, 1, 502, 
                            internAtom("Server dropped connection"));
        return 1;
    }

    return 0;
}

int
httpServerHandlerHeaders(int eof,
                         FdEventHandlerPtr event,
                         StreamRequestPtr srequest, 
                         HTTPConnectionPtr connection)
{
    HTTPRequestPtr request = connection->request;
    ObjectPtr object = request->object;
    int rc;
    int code, version;
    int full_len;
    AtomPtr headers;
    int len;
    int te;
    CacheControlRec cache_control;
    int age = -1;
    time_t date, last_modified, expires;
    struct timeval *init_time;
    char *etag;
    AtomPtr via, new_via;
    int expect_body;
    HTTPRangeRec content_range;
    ObjectPtr new_object = NULL, old_object = NULL;
    int supersede = 0;
    AtomPtr message = NULL;
    int suspectDynamic;
    AtomPtr url = NULL;

    assert(request->object->flags & OBJECT_INPROGRESS);
    assert(eof >= 0);

    httpSetTimeout(connection, -1);

    if(request->wait_continue) {
        do_log(D_SERVER_CONN, "W   %s:%d.\n",
               connection->server->name, connection->server->port);
        request->wait_continue = 0;
    }

    rc = httpParseServerFirstLine(connection->buf, &code, &version, &message);
    if(rc <= 0) {
        do_log(L_ERROR, "Couldn't parse server status line.\n");
        httpServerAbort(connection, 1, 502,
                        internAtom("Couldn't parse server status line"));
        return 1;
    }

    do_log(D_SERVER_REQ, "Server status: ");
    do_log_n(D_SERVER_REQ, connection->buf, 
             connection->buf[rc - 1] == '\r' ? rc - 2 : rc - 2);
    do_log(D_SERVER_REQ, " (0x%lx for 0x%lx)\n",
           (unsigned long)connection, (unsigned long)object);

    if(version != HTTP_10 && version != HTTP_11) {
        do_log(L_ERROR, "Unknown server HTTP version\n");
        httpServerAbort(connection, 1, 502,
                        internAtom("Unknown server HTTP version"));
        releaseAtom(message);
        return 1;
    } 

    connection->version = version;
    connection->server->version = version;
    request->persistent = 1;

    url = internAtomN(object->key, object->key_size);    
    rc = httpParseHeaders(0, url, connection->buf, rc, request,
                          &headers, &len, &cache_control, NULL, &te,
                          &date, &last_modified, &expires, NULL, NULL, NULL,
                          &age, &etag, NULL, NULL, &content_range,
                          NULL, &via, NULL);
    if(rc < 0) {
        do_log(L_ERROR, "Couldn't parse server headers\n");
        releaseAtom(url);
        releaseAtom(message);
        httpServerAbort(connection, 1, 502, 
                        internAtom("Couldn't parse server headers"));
        return 1;
    }

    if(code == 100) {
        releaseAtom(url);
        releaseAtom(message);
        /* We've already reset wait_continue above, but we must still
           ensure that the writer notices. */
        notifyObject(request->object);
        httpServerFinish(connection, -1, rc);
        return 1;
    }

    if(code == 101) {
        httpServerAbort(connection, 1, 501,
                        internAtom("Upgrade not implemented"));
        goto fail;
    }

    if(via && !checkVia(proxyName, via)) {
        httpServerAbort(connection, 1, 504, internAtom("Proxy loop detected"));
        goto fail;
    }
    full_len = content_range.full_length;

    if(code == 206) {
        if(content_range.from == -1 || content_range.to == -1) {
            do_log(L_ERROR, "Partial content without range.\n");
            httpServerAbort(connection, 1, 502,
                            internAtom("Partial content without range"));
            goto fail;
        }
        if(len >= 0 && len != content_range.to - content_range.from) {
            do_log(L_ERROR, "Inconsistent partial content.\n");
            httpServerAbort(connection, 1, 502,
                            internAtom("Inconsistent partial content"));
            goto fail;
        }
    } else if(code < 400 && 
              (content_range.from >= 0 || content_range.to >= 0 || 
               content_range.full_length >= 0)) {
        do_log(L_ERROR, "Range without partial content.\n");
        httpServerAbort(connection, 1, 502,
                        internAtom("Range without partial content"));
        goto fail;
    } else if(code != 304 && code != 412) {
        full_len = len;
    }

    if(te != TE_IDENTITY && te != TE_CHUNKED) {
        do_log(L_ERROR, "Unsupported transfer-encoding\n");
        httpServerAbort(connection, 1, 502,
                        internAtom("Unsupported transfer-encoding"));
        goto fail;
    }

    if(code == 304) {
        if(request->method != METHOD_CONDITIONAL_GET) {
            do_log(L_ERROR, "Unexpected \"not changed\" reply from server\n");
            httpServerAbort(connection, 1, 502,
                            internAtom("Unexpected \"not changed\" "
                                       "reply from server"));
            goto fail;
        }
        if(object->etag && !etag) {
            /* RFC 2616 10.3.5.  Violated by some front-end proxies. */
            do_log(L_WARN, "\"Not changed\" reply with no ETag.\n");
        } 
    }

    if(code == 412) {
        if(request->method != METHOD_CONDITIONAL_GET) {
            do_log(L_ERROR, 
                   "Unexpected \"precondition failed\" reply from server.\n");
            httpServerAbort(connection, 1, 502,
                            internAtom("Unexpected \"precondition failed\" "
                                       "reply from server"));
            goto fail;
        }
        assert(object->etag || object->last_modified);
    }

    releaseAtom(url);

    /* Okay, we're going to accept this reply. */

    if((code == 200 || code == 206 || code == 304 || code == 412) &&
       (cache_control.flags & (CACHE_NO | CACHE_NO_STORE) ||
        cache_control.max_age == 0 ||
        (cacheIsShared && cache_control.s_maxage == 0) ||
        (expires >= 0 && expires <= object->age))) {
        do_log(L_UNCACHEABLE, "Uncacheable object ");
        do_log_n(L_UNCACHEABLE, object->key, object->key_size);
        do_log(L_UNCACHEABLE, " (%d)\n", cache_control.flags);
    }

    if(request->time0.tv_sec != null_time.tv_sec)
        init_time = &request->time0;
    else
        init_time = &current_time;
    age = MIN(init_time->tv_sec - age, init_time->tv_sec);

    if(request->method == METHOD_HEAD || 
       code < 200 || code == 204 || code == 304)
        expect_body = 0;
    else if(te == TE_IDENTITY)
        expect_body = (len != 0);
    else
        expect_body = 1;

    connection->chunk_remaining = -1;
    connection->te = te;

    old_object = object;

    connection->server->lies--;

    if(code == 304 || code == 412) {
        if((object->etag && etag && strcmp(object->etag, etag) != 0) ||
           (object->last_modified >= 0 && last_modified >= 0 &&
            object->last_modified != last_modified)) {
            do_log(L_ERROR, "Inconsistent \"%s\" reply for ",
                   code == 304 ? "not changed":"precondition failed");
            do_log_n(L_ERROR, object->key, object->key_size);
            do_log(L_ERROR, "\n");
            object->flags |= OBJECT_DYNAMIC;
            supersede = 1;
        }
    } else if(!(object->flags & OBJECT_INITIAL)) {
        if((object->last_modified < 0 || last_modified < 0) &&
           (!object->etag || !etag))
            supersede = 1;
        else if(object->last_modified != last_modified)
            supersede = 1;
        else if(object->etag || etag) {
            /* We need to be permissive here so as to deal with some
               front-end proxies that discard ETags on partial
               replies but not on full replies. */
            if(etag && object->etag && strcmp(object->etag, etag) != 0)
                supersede = 1;
            else if(!object->etag)
                supersede = 1;
        }

        if(!supersede && object->length >= 0 && full_len >= 0 &&
                object->length != full_len) {
            do_log(L_WARN, "Inconsistent length.\n");
            supersede = 1;
        }

        if(!supersede &&
           ((object->last_modified >= 0 && last_modified >= 0) ||
            (object->etag && etag))) {
            if(request->method == METHOD_CONDITIONAL_GET) {
                do_log(L_WARN, "Server ignored conditional request.\n");
                connection->server->lies += 10;
                /* Drop the connection? */
            }
        }
    } else if(code == 416) {
        do_log(L_ERROR, "Unexpected \"range not satisfiable\" reply\n");
        httpServerAbort(connection, 1, 502,
                        internAtom("Unexpected \"range not satisfiable\" "
                                   "reply"));
        /* The object may be superseded.  Make sure the next request
           won't be partial. */
        abortObject(object, 502, 
                    internAtom("Unexpected \"range not satisfiable\" reply"));
        return 1;
    }

    if(object->flags & OBJECT_INITIAL)
        supersede = 0;

    if(supersede) {
        do_log(L_SUPERSEDED, "Superseding object: ");
        do_log_n(L_SUPERSEDED, old_object->key, old_object->key_size);
        do_log(L_SUPERSEDED, " (%d %d %d %s -> %d %d %d %s)\n",
               object->code, object->length, (int)object->last_modified,
               object->etag?object->etag: "(none)",
               code, full_len, (int)last_modified,
               etag?etag:"(none)");
        privatiseObject(old_object, 0);
        new_object = makeObject(object->type, object->key, 
                                object->key_size, 1, 0, 
                                object->request, NULL);
        if(new_object == NULL) {
            do_log(L_ERROR, "Couldn't allocate object\n");
            httpServerAbort(connection, 1, 500,
                            internAtom("Couldn't allocate object"));
            return 1;
        }
        if(urlIsLocal(new_object->key, new_object->key_size))
            new_object->flags |= OBJECT_LOCAL;
    } else {
        new_object = object;
    }

    suspectDynamic =
        (!etag && last_modified < 0) ||
        (cache_control.flags &
         (CACHE_NO_HIDDEN | CACHE_NO | CACHE_NO_STORE |
          (cacheIsShared ? CACHE_PRIVATE : 0))) ||
        (cache_control.max_age >= 0 && cache_control.max_age <= 2) ||
        (cacheIsShared && 
         cache_control.s_maxage >= 0 && cache_control.s_maxage <= 5) ||
        (old_object->last_modified >= 0 && old_object->expires >= 0 && 
         (old_object->expires - old_object->last_modified <= 1)) ||
        (supersede && (old_object->date - date <= 5));

    if(suspectDynamic)
        new_object->flags |= OBJECT_DYNAMIC;
    else if(!supersede)
        new_object->flags &= ~OBJECT_DYNAMIC;
    else if(old_object->flags & OBJECT_DYNAMIC)
        new_object->flags |= OBJECT_DYNAMIC;

    new_object->age = age;
    new_object->cache_control |= cache_control.flags;
    new_object->max_age = cache_control.max_age;
    new_object->s_maxage = cache_control.s_maxage;
    new_object->flags &= ~OBJECT_FAILED;

    if(date >= 0)
        new_object->date = date;
    if(last_modified >= 0)
        new_object->last_modified = last_modified;
    if(expires >= 0)
        new_object->expires = expires;
    if(new_object->etag == NULL)
        new_object->etag = etag;
    else
        free(etag);

    switch(code) {
    case 200:
    case 300: case 301: case 302: case 303: case 307:
    case 403: case 404: case 405: case 401:
        if(new_object->message) releaseAtom(new_object->message);
        new_object->code = code;
        new_object->message = message;
        break;
    case 206: case 304: case 412:
        if(new_object->code != 200 || !new_object->message) {
            if(new_object->message) releaseAtom(new_object->message);
            new_object->code = 200;
            new_object->message = internAtom("OK");
        }
        releaseAtom(message);
        break;
    default:
        if(new_object->message) releaseAtom(new_object->message);
        new_object->code = code;
        new_object->message = retainAtom(message);
        break;
    }

    if((cache_control.flags & CACHE_AUTHORIZATION) &&
       !(cache_control.flags & CACHE_PUBLIC))
        new_object->cache_control |= (CACHE_NO_HIDDEN | OBJECT_LINEAR);

    /* This is not required by RFC 2616 -- but see RFC 3143 2.1.1.  We
       manically avoid caching replies that we don't know how to
       handle, even if Expires or Cache-Control says otherwise.  As to
       known uncacheable replies, we obey Cache-Control and default to
       allowing sharing but not caching. */
    if(code != 200 && code != 206 && 
       code != 300 && code != 301 && code != 302 && code != 303 &&
       code != 304 && code != 307 &&
       code != 403 && code != 404 && code != 405 && code != 416) {
        new_object->cache_control |= (CACHE_NO_HIDDEN | OBJECT_LINEAR);
    } else if(code != 200 && code != 206 &&
              code != 300 && code != 302 && code != 304 &&
              code != 410) {
        if(new_object->expires < 0 && !(cache_control.flags & CACHE_PUBLIC)) {
            new_object->cache_control |= CACHE_NO_HIDDEN;
        }
    }

    if(!via)
        new_via = internAtomF("%s %s",
                              version == HTTP_11 ? "1.1" : "1.0",
                              proxyName->string);
    else
        new_via = internAtomF("%s, %s %s", via->string,
                              version == HTTP_11 ? "1.1" : "1.0",
                              proxyName->string);
    if(new_via == NULL) {
        do_log(L_ERROR, "Couldn't allocate Via.\n");
    } else {
        if(new_object->via) releaseAtom(new_object->via);
        new_object->via = new_via;
    }

    if((new_object->cache_control & CACHE_NO_STORE) ||
       ((new_object->cache_control & CACHE_VARY) && !new_object->etag))
        new_object->cache_control |= CACHE_NO_HIDDEN;

    if(new_object->flags & OBJECT_INITIAL) {
        objectPartial(new_object, full_len, headers);
    } else {
        if(new_object->length < 0)
            new_object->length = full_len;
        /* XXX -- RFC 2616 13.5.3 */
        releaseAtom(headers);
    }

    if(supersede) {
        assert(new_object != old_object);
        supersedeObject(old_object);
    }

    if(new_object != old_object) {
        if(new_object->flags & OBJECT_INPROGRESS) {
            /* Make sure we don't fetch this object two times at the
               same time.  Just drop the connection. */
            releaseObject(new_object);
            httpServerFinish(connection, 1, 0);
            return 1;
        }
        old_object->flags &= ~OBJECT_VALIDATING;
        new_object->flags |= OBJECT_INPROGRESS;
        /* Signal the client side to switch to the new object -- see
           httpClientGetHandler.  If it doesn't, we'll give up on this
           request below. */
        new_object->flags |= OBJECT_MUTATING;
        request->can_mutate = new_object;
        notifyObject(old_object);
        request->can_mutate = NULL;
        new_object->flags &= ~OBJECT_MUTATING;
        old_object->flags &= ~OBJECT_INPROGRESS;
        if(request->object == old_object) {
            if(request->request)
                request->request->request = NULL;
            request->request = NULL;
            request->object = new_object;
        } else {
            assert(request->object == new_object);
        }
        releaseNotifyObject(old_object);
        old_object = NULL;
        object = new_object;
    } else {
        objectMetadataChanged(new_object, 0);
    }

    if(object->flags & OBJECT_VALIDATING) {
        object->flags &= ~OBJECT_VALIDATING;
        notifyObject(object);
    }

    if(!expect_body) {
        httpServerFinish(connection, 0, rc);
        return 1;
    }

    if(request->request == NULL) {
        httpServerFinish(connection, 1, 0);
        return 1;
    }

    if(code == 412) {
        /* 412 replies contain a useless body.  For now, we
           drop the connection. */
        httpServerFinish(connection, 1, 0);
        return 1;
    }


    if(request->persistent) {
        if(request->method != METHOD_HEAD && 
           connection->te == TE_IDENTITY && len < 0) {
            do_log(L_ERROR, "Persistent reply with no Content-Length\n");
            /* That's potentially dangerous, as we could start reading
               arbitrary data into the object.  Unfortunately, some
               servers do that. */
            request->persistent = 0;
        }
    }

    /* we're getting a body */
    if(content_range.from > 0)
        connection->offset = content_range.from;
    else
        connection->offset = 0;

    if(content_range.to >= 0)
        request->to = content_range.to;

    do_log(D_SERVER_OFFSET, "0x%lx(0x%lx): offset = %d\n",
           (unsigned long)connection, (unsigned long)object,
           connection->offset);

    if(connection->len > rc) {
        rc = connectionAddData(connection, rc);
        if(rc) {
            if(rc < 0) {
                if(rc == -2) {
                    do_log(L_ERROR, "Couldn't parse chunk size.\n");
                    httpServerAbort(connection, 1, 502,
                                    internAtom("Couldn't parse chunk size"));
                } else {
                    do_log(L_ERROR, "Couldn't add data to connection.\n");
                    httpServerAbort(connection, 1, 500, 
                                    internAtom("Couldn't add data "
                                               "to connection"));
                }
                return 1;
            } else {
                if(code != 206) {
                    if(object->length < 0) {
                        object->length = object->size;
                        objectMetadataChanged(object, 0);
                    } else if(object->length != object->size) {
                        httpServerAbort(connection, 1, 500, 
                                        internAtom("Inconsistent "
                                                   "object size"));
                        object->length = -1;
                        return 1;
                    }
                }
                httpServerFinish(connection, 0, 0);
                return 1;
            }
        }
    } else {
        connection->len = 0;
    }

    if(eof) {
        if(connection->te == TE_CHUNKED ||
           (object->length >= 0 && 
            connection->offset < object->length)) {
            do_log(L_ERROR, "Server closed connection.\n");
            httpServerAbort(connection, 1, 502,
                            internAtom("Server closed connection"));
            return 1;
        } else {
            if(code != 206 && eof != -ECLIENTRESET && object->length < 0) {
                object->length = object->size;
                objectMetadataChanged(object, 0);
            }
            httpServerFinish(connection, 1, 0);
            return 1;
        }
    } else {
        return httpServerReadData(connection, 1);
    }
    return 0;

 fail:
    releaseAtom(url);
    releaseAtom(message);
    if(headers)
        releaseAtom(headers);
    if(etag)
        free(etag);
    if(via)
        releaseAtom(via);
    return 1;
}

int
httpServerIndirectHandlerCommon(HTTPConnectionPtr connection, int eof)
{
    HTTPRequestPtr request = connection->request;

    assert(eof >= 0);
    assert(request->object->flags & OBJECT_INPROGRESS);

    if(connection->len > 0) {
        int rc;
        rc = connectionAddData(connection, 0);
        if(rc) {
            if(rc < 0) {
                if(rc == -2) {
                    do_log(L_ERROR, "Couldn't parse chunk size.\n");
                    httpServerAbort(connection, 1, 502,
                                    internAtom("Couldn't parse chunk size"));
                } else {
                    do_log(L_ERROR, "Couldn't add data to connection.\n");
                    httpServerAbort(connection, 1, 500,
                                    internAtom("Couldn't add data "
                                               "to connection"));
                }
                return 1;
            } else {
                if(request->to < 0) {
                    if(request->object->length < 0) {
                        request->object->length = request->object->size;
                        objectMetadataChanged(request->object, 0);
                    } else if(request->object->length != 
                              request->object->size) {
                        request->object->length = -1;
                        httpServerAbort(connection, 1, 502,
                                        internAtom("Inconsistent "
                                                   "object size"));
                        return 1;
                    }
                }
                httpServerFinish(connection, 0, 0);
            }
            return 1;
        }
    }

    if(eof && connection->len == 0) {
        if(connection->te == TE_CHUNKED ||
           (request->to >= 0 && connection->offset < request->to)) {
            do_log(L_ERROR, "Server dropped connection.\n");
            httpServerAbort(connection, 1, 502, 
                            internAtom("Server dropped connection"));
            return 1;
        } else {
            if(request->object->length < 0 && eof != -ECLIENTRESET &&
               (request->to < 0 || request->to > request->object->size)) {
                request->object->length = request->object->size;
                objectMetadataChanged(request->object, 0);
            }
            httpServerFinish(connection, 1, 0);
            return 1;
        }
    } else {
        return httpServerReadData(connection, 0);
    }
}

int
httpServerIndirectHandler(int status,
                          FdEventHandlerPtr event, 
                          StreamRequestPtr srequest)
{
    HTTPConnectionPtr connection = srequest->data;
    assert(connection->request->object->flags & OBJECT_INPROGRESS);

    httpSetTimeout(connection, -1);
    if(status < 0) {
        if(status != -ECLIENTRESET)
            do_log_error(L_ERROR, -status, "Read from server failed");
        httpServerAbort(connection, status != -ECLIENTRESET, 502,
                        internAtomError(-status, "Read from server failed"));
        return 1;
    }

    connection->len = srequest->offset;

    return httpServerIndirectHandlerCommon(connection, status);
}

int
httpServerReadData(HTTPConnectionPtr connection, int immediate)
{
    HTTPRequestPtr request = connection->request;
    ObjectPtr object = request->object;
    int to = -1;

    assert(object->flags & OBJECT_INPROGRESS);

    if(request->request == NULL) {
        httpServerFinish(connection, 1, 0);
        return 1;
    }

    if(request->to >= 0)
        to = request->to;
    else
        to = object->length;

    if(to >= 0 && to == connection->offset) {
        httpServerFinish(connection, 0, 0);
        return 1;
    }

    if(connection->len == 0 &&
       ((connection->te == TE_IDENTITY && to > connection->offset) ||
        (connection->te == TE_CHUNKED && connection->chunk_remaining > 0))) {
        /* Read directly into the object */
        int i = connection->offset / CHUNK_SIZE;
        int j = connection->offset % CHUNK_SIZE;
        int end, len, more;
        /* See httpServerDirectHandlerCommon if you change this */
        if(connection->te == TE_CHUNKED) {
            len = connection->chunk_remaining;
            /* The logic here is that we want more to just fit the
               chunk header if we're doing a large read, but do a
               large read if we would otherwise do a small one.  The
               magic constant 2000 comes from the assumption that the
               server uses chunks that have a size that are a power of
               two (possibly including the chunk header), and that we
               want a full ethernet packet to fit into our read. */
            more = (len >= 2000 ? 20 : MIN(2048 - len, CHUNK_SIZE));
        } else {
            len = to - connection->offset;
            /* We read more data only when there is a reasonable
               chance of there being another reply coming. */
            more = (connection->pipelined > 1) ? CHUNK_SIZE : 0;
        }
        end = len + connection->offset;

        if(connection->buf) {
            dispose_chunk(connection->buf);
            connection->buf = NULL;
        }

        /* The order of allocation is important in case we run out of
           memory. */
        lockChunk(object, i);
        if(object->chunks[i].data == NULL)
            object->chunks[i].data = get_chunk();
        if(object->chunks[i].data && object->chunks[i].size >= j) {
            if(len + j > CHUNK_SIZE) {
                lockChunk(object, i + 1);
                if(object->chunks[i + 1].data == NULL)
                    object->chunks[i + 1].data = get_chunk();
                /* Unless we're grabbing all len of data, we do not
                   want to do an indirect read immediately afterwards. */
                if(more && len + j <= 2 * CHUNK_SIZE) {
                    if(!connection->buf)
                        connection->buf = get_chunk(); /* checked below */
                }
                if(object->chunks[i + 1].data) {
                    do_stream_3(IO_READ | IO_NOTNOW, connection->fd, j,
                                object->chunks[i].data, CHUNK_SIZE,
                                object->chunks[i + 1].data,
                                MIN(CHUNK_SIZE,
                                    end - (i + 1) * CHUNK_SIZE),
                                connection->buf, connection->buf ? more : 0,
                                httpServerDirectHandler2, connection);
                    return 1;
                }
                unlockChunk(object, i + 1);
            }
            if(more && len + j <= CHUNK_SIZE) {
                if(!connection->buf)
                    connection->buf = get_chunk();
            }
            do_stream_2(IO_READ | IO_NOTNOW, connection->fd, j,
                        object->chunks[i].data,
                        MIN(CHUNK_SIZE, end - i * CHUNK_SIZE),
                        connection->buf, connection->buf ? more : 0,
                        httpServerDirectHandler, connection);
            return 1;
        } else {
            unlockChunk(object, i);
        }
    }
       
    if(connection->len == 0) {
        if(connection->buf) {
            dispose_chunk(connection->buf);
            connection->buf = NULL;
        }
    }

    httpSetTimeout(connection, 60);
    do_stream_buf(IO_READ | IO_NOTNOW |
                  ((immediate && connection->len) ? IO_IMMEDIATE : 0),
                  connection->fd, connection->len,
                  &connection->buf,
                  (connection->te == TE_CHUNKED ? 
                   MIN(2048, CHUNK_SIZE) : CHUNK_SIZE),
                  httpServerIndirectHandler, connection);
    return 1;
}

int
httpServerDirectHandlerCommon(int kind, int status,
                              FdEventHandlerPtr event, 
                              StreamRequestPtr srequest)
{
    HTTPConnectionPtr connection = srequest->data;
    HTTPRequestPtr request = connection->request;
    ObjectPtr object = request->object;
    int i = connection->offset / CHUNK_SIZE;
    int to, end, end1;

    assert(request->object->flags & OBJECT_INPROGRESS);

    httpSetTimeout(connection, -1);

    if(status < 0) {
        unlockChunk(object, i);
        if(kind == 2) unlockChunk(object, i + 1);
        if(status != -ECLIENTRESET)
            do_log_error(L_ERROR, -status, "Read from server failed");
        httpServerAbort(connection, status != -ECLIENTRESET, 502,
                        internAtomError(-status, "Read from server failed"));
        return 1;
    }

    /* We have incestuous knowledge of the decisions made in
       httpServerReadData */
    if(request->to >= 0)
        to = request->to;
    else
        to = object->length;
    if(connection->te == TE_CHUNKED)
        end = connection->offset + connection->chunk_remaining;
    else
        end = to;
    /* The amount of data actually read into the object */
    end1 = MIN(end, i * CHUNK_SIZE + MIN(kind * CHUNK_SIZE, srequest->offset));

    assert(end >= 0 && end1 >= i * CHUNK_SIZE && end1 <= (i + 2) * CHUNK_SIZE);

    object->chunks[i].size = 
        MAX(object->chunks[i].size, MIN(end1 - i * CHUNK_SIZE, CHUNK_SIZE));
    if(kind == 2 && end1 > (i + 1) * CHUNK_SIZE) {
        object->chunks[i + 1].size =
            MAX(object->chunks[i + 1].size, end1 - (i + 1) * CHUNK_SIZE);
    }
    if(connection->te == TE_CHUNKED) {
        connection->chunk_remaining -= (end1 - connection->offset);
        assert(connection->chunk_remaining >= 0);
    }
    connection->offset = end1;
    object->size = MAX(object->size, end1);
    unlockChunk(object, i);
    if(kind == 2) unlockChunk(object, i + 1);

    if(i * CHUNK_SIZE + srequest->offset > end1) {
        connection->len = i * CHUNK_SIZE + srequest->offset - end1;
        return httpServerIndirectHandlerCommon(connection, status);
    } else {
        notifyObject(object);
        if(status) {
            httpServerFinish(connection, 1, 0);
            return 1;
        } else {
            return httpServerReadData(connection, 0);
        }
    }
}

int
httpServerDirectHandler(int status,
                        FdEventHandlerPtr event, 
                        StreamRequestPtr srequest)
{
    return httpServerDirectHandlerCommon(1, status, event, srequest);
}
    
int
httpServerDirectHandler2(int status,
                         FdEventHandlerPtr event, 
                         StreamRequestPtr srequest)
{
    return httpServerDirectHandlerCommon(2, status, event, srequest);
}

/* Add the data accumulated in connection->buf into the object in
   connection->request.  Returns 0 in the normal case, 1 if the TE is
   self-terminating and we're done, -1 if there was a problem with
   objectAddData, -2 if there was a problem with the data. */
int
connectionAddData(HTTPConnectionPtr connection, int skip)
{
    HTTPRequestPtr request = connection->request;
    ObjectPtr object = request->object;
    int rc;

    if(connection->te == TE_IDENTITY) {
        int len;
        
        len = connection->len - skip;
        if(object->length >= 0) {
            len = MIN(object->length - connection->offset, len);
        }
        if(request->to >= 0)
            len = MIN(request->to - connection->offset, len);
        if(len > 0) {
            rc = objectAddData(object, connection->buf + skip,
                               connection->offset, len);
            if(rc < 0)
                return -1;
            connection->offset += len;
            connection->len -= (len + skip);
            do_log(D_SERVER_OFFSET, "0x%lx(0x%lx): offset = %d\n",
                   (unsigned long)connection, (unsigned long)object,
                   connection->offset);
        }

        if(connection->len > 0 && skip + len > 0) {
            memmove(connection->buf,
                    connection->buf + skip + len, connection->len);
        }

        if((object->length >= 0 && object->length <= connection->offset) ||
           (request->to >= 0 && request->to <= connection->offset)) {
            notifyObject(object);
            return 1;
        } else {
            if(len > 0)
                notifyObject(object);
            return 0;
        }
    } else if(connection->te == TE_CHUNKED) {
        int i = skip, j, size;
        /* connection->chunk_remaining is 0 at the end of a chunk, -1
           after the CR/LF pair ending a chunk, and -2 after we've
           seen a chunk of length 0. */
        if(connection->chunk_remaining > -2) {
            while(1) {
                if(connection->chunk_remaining <= 0) {
                    if(connection->chunk_remaining == 0) {
                        if(connection->len < i + 2)
                            break;
                        if(connection->buf[i] != '\r' ||
                           connection->buf[i + 1] != '\n')
                            return -1;
                        i += 2;
                        connection->chunk_remaining = -1;
                    }
                    if(connection->len < i + 2)
                        break;
                    j = parseChunkSize(connection->buf, i,
                                       connection->len, &size);
                    if(j < 0)
                        return -2;
                    if(j == 0)
                        break;
                    else
                        i = j;
                    if(size == 0) {
                        connection->chunk_remaining = -2;
                        break;
                    } else {
                        connection->chunk_remaining = size;
                    }
                } else {
                    /* connection->chunk_remaining > 0 */
                    size = MIN(connection->chunk_remaining,
                               connection->len - i);
                    if(size <= 0)
                        break;
                    rc = objectAddData(object, connection->buf + i,
                                       connection->offset, size);
                    connection->offset += size;
                    if(rc < 0)
                        return -1;
                    i += size;
                    connection->chunk_remaining -= size;
                    do_log(D_SERVER_OFFSET, "0x%lx(0x%lx): offset = %d\n",
                           (unsigned long)connection, 
                           (unsigned long)object,
                           connection->offset);
                }
            }
        }
        connection->len -= i;
        if(connection->len > 0)
            memmove(connection->buf, connection->buf + i, connection->len);
        if(i > 0 || connection->chunk_remaining == -2)
            notifyObject(object);
        if(connection->chunk_remaining == -2)
            return 1;
        else
            return 0;
    } else {
        abort();
    }
}

void
listServers()
{
    HTTPServerPtr server;
    int i, n, m;

    printf("<!DOCTYPE HTML PUBLIC "
           "\"-//W3C//DTD HTML 4.01 Transitional//EN\" "
           "\"http://www.w3.org/TR/html4/loose.dtd\">\n"
           "<html><head>\n"
           "\r\n<title>Known servers</title>\n"
           "</head><body>\n"
           "<h1>Known servers</h1>\n");

    printf("<table>\n");
    printf("<thead><tr><th>Server</th>"
           "<th>Version</th>"
           "<th>Persistent</th>"
           "<th>Pipeline</th>"
           "<th>Connections</th>"
           "<th></th>"
           "<th>rtt</th>"
           "<th>rate</th>"
           "</tr></thead>\n");
    printf("<tbody>\n");
    server = servers;
    while(server) {
        printf("<tr>");

        if(server->port == 80)
            printf("<td>%s</td>", server->name);
        else
            printf("<td>%s:%d</td>", server->name, server->port);

        if(server->version == HTTP_11)
            printf("<td>1.1</td>");
        else if(server->version == HTTP_10)
            printf("<td>1.0</td>");
        else
            printf("<td>unknown</td>");

        if(server->persistent < 0)
            printf("<td>no</td>");
        else if(server->persistent > 0)
            printf("<td>yes</td>");
        else
            printf("<td>unknown</td>");

        if(server->version != HTTP_11 || server->persistent <= 0)
            printf("<td></td>");
        else if(server->pipeline < 0)
            printf("<td>no</td>");
        else if(server->pipeline >= 0 && server->pipeline <= 1)
            printf("<td>unknown</td>");
        else if(server->pipeline == 2 || server->pipeline == 3)
            printf("<td>probing</td>");
        else 
            printf("<td>yes</td>");

        n = 0; m = 0;
        for(i = 0; i < server->maxslots; i++)
            if(server->connection[i] && !server->connection[i]->connecting) {
                if(i < server->numslots)
                    n++;
                else
                    m++;
            }
            
        printf("<td>%d/%d", n, server->numslots);
        if(m)
            printf(" + %d</td>", m);
        else
            printf("</td>");

        if(server->lies > 0)
            printf("<td>(%d lies)</td>", (server->lies + 9) / 10);
        else
            printf("<td></td>");

        if(server->rtt > 0)
            printf("<td>%.3f</td>", (double)server->rtt / 1000000.0);
        else
            printf("<td></td>");
        if(server->rate > 0)
            printf("<td>%d</td>", server->rate);
        else
            printf("<td></td>");
        
        printf("</tr>\n");
        if(feof(stdout))
            exit(1);
        server = server->next;
    }
    printf("</tbody>\n");
    printf("</table>\n");
    printf("<p><a href=\"/polipo/\">back</a></p>");
    printf("</body></html>\n");
    exit(0);
}
