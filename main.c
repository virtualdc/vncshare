#include <stdio.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <malloc.h>
#include <string.h>
#include <stdlib.h>
#include <poll.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/time.h>


// Port for incoming connections
int serverPort = 5500;


struct ThreadParams
{
    int socket;
    struct sockaddr_in addr;
};


// log printf
void lprintf(const char *format, ...)
{
    time_t time1;
    struct tm time2;
    char buf1[30], buf2[1024];
    va_list va;

    time1 = time(NULL);
    localtime_r(&time1, &time2);
    strftime(buf1, 30, "%Y-%m-%d %H:%M:%S", &time2);
    va_start(va, format);
    vsprintf(buf2, format, va);
    va_end(va);
    fprintf(stderr, "%s %s\n", buf1, buf2);
}


// read size bytes from fd into buffer
// returns 0 on success, -1 on failure
int preciseRead(int fd, char *buf, int size)
{
    while (size)
    {
        int readed = read(fd, buf, size);
        if (readed < 0)
        {
            lprintf("[%d] Read error on socket: %d (%s)",
                   fd, errno, strerror(errno));
            return -1;
        }
        else if (readed == 0)
        {
            lprintf("[%d] Connection unexpectly closed", fd);
            return -1;
        }
        buf += readed;
        size -= readed;
    }
    return 0;
}


// Read identification packet
int readID(int fd, char *id, int maxIdSize)
{
    char buf[0xFA];

    // read 0xFA bytes
    if (preciseRead(fd, buf, 0xFA) != 0){
        return -1;
    }

    // check magic of ID packet
    if (buf[0] != 'I' && buf[1] != 'D' && buf[2] != ':')
    {
        lprintf("[%d] Invalid magic in ID packet.", fd);
        return -1;
    }

    // copy string to output buffer
    strncpy(id, buf+3, maxIdSize-1);
    id[maxIdSize-1] = 0;

    return 0;

}


// locate operator in operators.conf
// returns 0 if ok, -1 on failure
int locateOperator(const char *id, struct sockaddr_in *addr)
{
    char buf[256];
    int line;

    // open operators.conf
    FILE *f = fopen("operators.conf", "rt");
    if (!f)
    {
        lprintf("Can't open operators.conf");
        return -1;
    }
    // read file line by line
    line = 0;
    while (1)
    {
        char *opId, *opIP, *opPort, *tmp;
        if (!fgets(buf, 256, f)) break;
        line++;
        opId = strtok_r(buf, " \t", &tmp);
        opIP = strtok_r(NULL, " \t", &tmp);
        opPort = strtok_r(NULL, " \t\r\n", &tmp);
        if (opId == NULL || opIP == NULL || opPort == NULL)
        {
            lprintf("WARN: Bad line %d in operators.conf", line);
            fclose(f);
            return -1;
        }
        if (!strcmp(id, opId))
        {
            char *endptr;
            int port;
            if (inet_aton(opIP, &addr->sin_addr) == 0)
            {
                lprintf("Bad IP address %s in operators.conf", opIP);
                fclose(f);
                return -1;
            }
            port = strtol(opPort, &endptr, 10);
            if (*endptr != 0) {
                lprintf("Bad port %s in operators.conf", opPort);
                fclose(f);
                return -1;
            }
            addr->sin_port = htons(port);
            addr->sin_family = AF_INET;
            return 0;
        }
    }

    lprintf("Can't find ID \"%s\" in operators.conf", id);
    fclose(f);
    return -1;
}


// connects to target and returns socket or -1
int connectToTarget(int sock, struct sockaddr_in *addr)
{
    int fd = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == -1)
    {
        lprintf("[%d] Can't create socket. Error %d (%s)", sock, errno, strerror(errno));
        return -1;
    }
    if (connect(fd, (struct sockaddr*)addr, sizeof(struct sockaddr_in)) == -1)
    {
        lprintf("[%d] Can't connect. Error %d (%s)", sock, errno, strerror(errno));
        return -1;
    }
    return fd;
}


// transfer chunk of data from fd1 to fd2
// return 0 on ok, -1 on failure
int transferChunk(int fd1, int fd2, char *buf, int *rest)
{
    int readed, written;

    if (*rest == 0)
    {
        // read some bytes
        readed = read(fd1, buf, 4096);
        if (readed < 0)
        {
            if (errno == EWOULDBLOCK || errno == EAGAIN)
            {
                return 0;
            }
            lprintf("[%d-%d] Error while reading: %d (%s)",
                   fd1, fd2, errno, strerror(errno));
            return -1;
        }
        if (readed == 0)
        {
            lprintf("[%d-%d] Connection closed by peer", fd1, fd2);
                return -1;
        }
    }
    else
    {
        // reuse bytes from previous read
        readed = *rest;
    }

    // write them to another side
    written = write(fd2, buf, readed);
    if (written < 0)
    {
        if (errno != EWOULDBLOCK && errno != EAGAIN)
        {
            lprintf("[%d-%d] Error while writing: %d (%s)",
                   fd1, fd2, errno, strerror(errno));
            return -1;
        }
        // nothing written
        written = 0;
    }
    else if (written == 0)
    {
        lprintf("[%d-%d] Connection closed by peer", fd1, fd2);
        return -1;
    }

    // calculate rest and move buffer if some bytes not written
    *rest = readed - written;
    if (*rest)
    {
        memmove(buf, buf+written, *rest);
    }

    return 0;

}


// transfer data between fd1 and fd2, return when error occured
// or connection closed by one of peers
void transfer(int fd1, int fd2)
{
    struct pollfd p[2];
    char buf1[4096], buf2[4096];
    int rest1 = 0, rest2 = 0;

    // switch sockets to nonblocking mode
    if (fcntl(fd1, F_SETFL, O_NONBLOCK) == -1) {
        lprintf("[%d] Can't switch to nonblocking mode. Error %d (%s).",
                fd1, errno, strerror(errno));
        return;
    }
    if (fcntl(fd2, F_SETFL, O_NONBLOCK) == -1) {
        lprintf("[%d] Can't switch to nonblocking mode. Error %d (%s).",
                fd2, errno, strerror(errno));
        return;
    }

    lprintf("[%d] Starting data transfer with %d", fd1, fd2);

    p[0].fd = fd1;
    p[0].events = POLLIN;
    p[1].fd = fd2;
    p[1].events = POLLIN;

    while (1)
    {
        int res = poll(p, 2, -1);
        if (res < 0)
        {
            lprintf("[%d] Poll error: %d (%s)", fd1, errno, strerror(errno));
            return;
        }
        if (p[0].revents & POLLIN || rest1 > 0)
        {
            if (transferChunk(fd1, fd2, buf1, &rest1) == -1) return;
        }
        if (p[1].revents & POLLIN || rest2 > 0)
        {
            if (transferChunk(fd2, fd1, buf2, &rest2) == -1) return;
        }
    }
}


// Incoming connections handler
void *handlerThread(void *arg)
{
    struct ThreadParams *params = (struct ThreadParams*)arg;
    char id[17];

    lprintf("[%d] Handler thread started for %s:%d",
           params->socket, inet_ntoa(params->addr.sin_addr),
           ntohs(params->addr.sin_port));

    // read ID
    if (readID(params->socket, id, 17) == 0)
    {
        struct sockaddr_in addr;
        lprintf("[%d] Client ID is \"%s\"", params->socket, id);
        if (locateOperator(id, &addr) == 0)
        {
            int target;
            lprintf("[%d] Connecting to target %s:%d",
                   params->socket, inet_ntoa(addr.sin_addr),
                   ntohs(addr.sin_port));
            target = connectToTarget(params->socket, &addr);
            if (target != -1)
            {
                transfer(params->socket, target);
                close(target);
            }
        }
    }

    lprintf("[%d] Handler thread stopped", params->socket);

    // cleanup
    close(params->socket);
    free(arg);
    return 0;
}



// Create socket and bind it
// Never returns on success
int startListener(int port)
{
    int fd;
    struct sockaddr_in addr;

    // create server socket
    fd = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == -1)
    {
        lprintf("Can't create socket. Error %d (%s)", errno, strerror(errno));
        return -1;
    }

    // bind socket
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(fd, (struct sockaddr*)&addr, sizeof(struct sockaddr_in)) != 0)
    {
        lprintf("Can't bind socket to port %d. Error %d (%s)",
               port, errno, strerror(errno));
        return -1;
    }

    // switch to listen mode
    if (listen(fd, 5) != 0)
    {
        lprintf("Can't listen on socket. Error %d (%s)",
               errno, strerror(errno));
        return -1;
    }

    // wait for connections
    lprintf("Waiting for incoming connections on port %d", port);
    while (1)
    {

        struct sockaddr_in addr;
        pthread_t th;
        struct ThreadParams *params;
        int error, client;
        unsigned int size = sizeof(struct sockaddr_in);

        // accept incoming connection
        client = accept(fd, (struct sockaddr*)&addr, &size);
        if (client == -1)
        {
            lprintf("Can't accept incoming connection. Error %d (%s)",
                   errno, strerror(errno));
            return -1;
        }

        lprintf("Client connected from %s:%d",
               inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));

        // start handling thread
        params = malloc(sizeof(struct ThreadParams));
        params->socket = client;
        memcpy(&params->addr, &addr, sizeof(struct sockaddr_in));
        error = pthread_create(&th, 0, handlerThread, params);
        if (error != 0)
        {
            free(params);
            lprintf("Can't create thread for client. Error %d (%s)",
                   error, strerror(error));
        }

    }

    return 0;
}


int main()
{

    if (startListener(serverPort) == -1)
    {
        lprintf("Fatal error occured. Program terminated.");
        return 1;
    }

    return 0;

}
