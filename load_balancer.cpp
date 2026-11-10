// ======================= TIME-TRAVEL DEBUGGER - LOAD BALANCER TEMPLATE =======================

// What load balancer does:
//   1. Many clients can connect at the same time. Each one sends one string (the source code).
//   2. Clients wait in a queue (first come, first served).
//   3. For each client, in order: make sure the server is running (start it with system() if not),
//      pass the string to the server, pass the server's reply back to the client.
//
// It does not read or understand the messages. It only moves bytes.
// The only messages it writes itself are errors ("server busy", "server failed to start").
//
// Two threads:
//   acceptor thread (acceptLoop) -> takes new clients and puts them in the queue. It never waits for the server.
//   main thread (dispatcherLoop) -> takes ONE client out of the queue and serves it completely, then the next.
//
// How the server is started:
//   system("./server 5001 &");
//   The "&" is required. Without it, system() waits until the server exits and the load balancer freezes.
//
//
// What the server program must do (the load balancer depends on this):
//   - it takes its port as the first argument, for example: ./server 5001
//   - it reads the string until the connection's write side is closed (recv returns 0)
//   - it sends its reply as [status: 1 byte][payload], then closes the connection

#include <iostream>
#include <string>
#include <cstdlib>
#include <thread>
#include <mutex>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "helper.h"
using namespace std;

// ---- Constants ----
const char *SERVER_PATH = "./server";      // the program we start when the server is down
const int LISTEN_BACKLOG = 16;             // connections the OS holds until accept() takes them
const int MAX_PENDING_CLIENTS = 8;         // clients allowed to wait in our queue. More than this get "server busy".
const int STARTUP_MAX_ATTEMPTS = 50;       // after starting the server, how many times we try to connect
const int STARTUP_POLL_INTERVAL_MS = 100;  // wait between those tries (50 x 100 ms = 5 seconds in total)
int lbPort = 5000;
string serverHost = "127.0.0.1";
int serverPort = 5001;

// ---- Queue: here it holds client sockets (int). ----
// Implement it yourself, do not use std::queue.
template <typename T>
class Queue
{
    struct Node
    {
        T data;
        Node *next;
    };
    Node *front, *rear;
    int count;

public:
    // Implement these functions:
    Queue()
    {
        front = nullptr;

        rear = nullptr;

        count = 0;
    }

    T dequeue()
    {
        if (front == nullptr)
        {
            return T();
        }

        Node *nodeToDelete = front;

        T extractedValue = nodeToDelete->data;

        front = front->next;

        if (front == nullptr)
        {
            rear = nullptr;
        }

        delete nodeToDelete;

        count--;

        return extractedValue;
    }

    ~Queue()
    {
        while (!isEmpty())
        {
            dequeue();
        }
    }

    void enqueue(const T &val)
    {
        Node *freshNode = new Node();

        freshNode->data = val;

        freshNode->next = nullptr;

        if (rear == nullptr)
        {
            front = freshNode;

            rear = freshNode;
        }
        else
        {
            rear->next = freshNode;

            rear = freshNode;
        }

        count++;
    }

    bool isEmpty()
    {
        return count == 0;
    }

    int size()
    {
        return count;
    }
};

// The queue is used by two threads, so every use of it must be inside queueMutex.
Queue<ClientInfo> pendingClients;
mutex queueMutex;
sig_atomic_t flag = 1;

// ---- Basic socket helpers ----
// Reply format: [status: 1 byte][sending_material][checksum: 4 bytes].
// send() may send fewer bytes than asked for, so keep calling it until all n bytes are sent.

bool sendError(int sock, const string &message)
{
    // Reply format: [status: 1 byte][payload until close][checksum: 4 bytes].
    // For an error: send one byte with value 1, then the message text.

    char error_status_byte = 1;

    uint32_t error_packet_crc = 0;

    error_packet_crc = computeRunningCRC(error_packet_crc, &error_status_byte, 1);

    if (!sendAll(sock, &error_status_byte, 1))
    {
        return false;
    }

    size_t error_text_length = message.length();

    if (error_text_length > 0)
    {
        error_packet_crc = computeRunningCRC(error_packet_crc, message.c_str(), error_text_length);

        if (!sendAll(sock, message.c_str(), error_text_length))
        {
            return false;
        }
    }

    if (!sendAll(sock, (const char *)&error_packet_crc, sizeof(uint32_t)))
    {
        return false;
    }

    return true;
}
void rejectClient(int sock, const string &message)
{
    // Send the error, then read and throw away what the client is still uploading, then close.
    // If we close while unread data is waiting, the OS resets the connection and the client never sees the error.

    sendError(sock, message);

    shutdown(sock, SHUT_WR);

    timeval wait_limit;

    wait_limit.tv_sec = 2;

    wait_limit.tv_usec = 0;

    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &wait_limit, sizeof(wait_limit));

    char discard_buffer[IO_BUFFER_SIZE];

    while (recv(sock, discard_buffer, sizeof(discard_buffer), 0) > 0)
    {
    }

    close(sock);
}
int createListenSocket(int port)
{
    // Create a TCP socket with SOCK_CLOEXEC(flag).
    // set SO_REUSEADDR so we can restart the load balancer right away.
    // bind() to the port on any address, then listen(LISTEN_BACKLOG).
    // If bind or listen fails, print the error and exit, because without this socket we cannot work.

    int listener_socket_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);

    if (listener_socket_fd < 0)
    {
        cerr << "failed to create listen socket" << endl;

        exit(1);
    }

    int reuse_address_flag = 1;

    setsockopt(listener_socket_fd, SOL_SOCKET, SO_REUSEADDR, &reuse_address_flag, sizeof(reuse_address_flag));

    sockaddr_in listener_address_info;

    memset(&listener_address_info, 0, sizeof(listener_address_info));

    listener_address_info.sin_family = AF_INET;

    listener_address_info.sin_port = htons((uint16_t)port);

    listener_address_info.sin_addr.s_addr = INADDR_ANY;

    int bind_result_code = bind(listener_socket_fd, (sockaddr *)&listener_address_info, sizeof(listener_address_info));

    if (bind_result_code < 0)
    {
        cerr << "bind failed on port " << port << endl;

        close(listener_socket_fd);

        exit(1);
    }

    int listen_result_code = listen(listener_socket_fd, LISTEN_BACKLOG);

    if (listen_result_code < 0)
    {
        cerr << "listen failed on port " << port << endl;

        close(listener_socket_fd);

        exit(1);
    }

    return listener_socket_fd;
}
int connectToServer(const char *host, int port)
{
    // Create a TCP socket (SOCK_CLOEXEC) and connect it to host:port.
    // Return the socket, or -1 if the connection fails

    int server_connection_sock = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);

    if (server_connection_sock < 0)
    {
        return -1;
    }

    sockaddr_in target_server_addr;

    memset(&target_server_addr, 0, sizeof(target_server_addr));

    target_server_addr.sin_family = AF_INET;

    target_server_addr.sin_port = htons((uint16_t)port);

    if (inet_pton(AF_INET, host, &target_server_addr.sin_addr) <= 0)
    {
        close(server_connection_sock);

        return -1;
    }

    int connection_status_code = connect(server_connection_sock, (sockaddr *)&target_server_addr, sizeof(target_server_addr));

    if (connection_status_code < 0)
    {
        close(server_connection_sock);

        return -1;
    }

    return server_connection_sock;
}

// =============================================================================================
// PART 1: WAKING UP THE SERVER IF IT IS DOWN
// =============================================================================================

bool startServer(int port)
{
    // Build the command text: SERVER_PATH + " " + port as text + "&"
    //   Example: "./server 5001 &"
    // Call system() with it.
    // Return true if system() returned 0, otherwise false.

    string launch_command_str = string(SERVER_PATH) + " " + to_string(port) + " &";

    int execution_exit_code = system(launch_command_str.c_str());

    if (execution_exit_code == 0)
    {
        return true;
    }

    return false;
}
int connectToServerWithWakeup()
{
    // 1. Try connectToServer(). If it works, return that socket. The server is alive.
    // 2. Otherwise call startServer(). If it fails, return -1.
    // 3. Try connectToServer() up to STARTUP_MAX_ATTEMPTS times
    // 4. Return the first working socket, or -1 if the server never came up.

    int initial_server_sock = connectToServer(serverHost.c_str(), serverPort);

    if (initial_server_sock >= 0)
    {
        return initial_server_sock;
    }

    if (!startServer(serverPort))
    {
        return -1;
    }

    for (int retry_count = 0; retry_count < STARTUP_MAX_ATTEMPTS; retry_count++)
    {
        usleep(STARTUP_POLL_INTERVAL_MS * 1000);

        int retry_server_sock = connectToServer(serverHost.c_str(), serverPort);

        if (retry_server_sock >= 0)
        {
            return retry_server_sock;
        }
    }

    return -1;
}

// =============================================================================================
// PART 2: PASSING THE STRING AND THE REPLY (one client, called by the main thread only)
// =============================================================================================

void relayOneClient(ClientInfo &c)
{
    // 1. serverSock = connectToServerWithWakeup().
    //    If it is -1: sendError "backend server failed to start", close clientSock, return.
    // 3. Client -> server: repeat recv() from clientSock into a buffer of IO_BUFFER_SIZE and sendAll() it to serverSock.
    //    Stop when recv() returns 0 (the client finished sending) or fails.
    // 4. Call shutdown(serverSock, SHUT_WR), so the server knows the string is complete.
    // 5. Server -> client: repeat recv() from serverSock and sendAll() to clientSock.
    //    Stop when recv() returns 0 (the server sent its whole reply and closed) or fails.
    // 6. Close both sockets.

    int server_sock = connectToServerWithWakeup();

    if (server_sock < 0)
    {
        rejectClient(c.sock, "backend server failed to start");

        return;
    }

    char buffer[IO_BUFFER_SIZE];

    timeval client_wait_limit;

    client_wait_limit.tv_sec = 5;

    client_wait_limit.tv_usec = 0;

    setsockopt(c.sock, SOL_SOCKET, SO_RCVTIMEO, &client_wait_limit, sizeof(client_wait_limit));

    while (true)
    {
        ssize_t received_count = recv(c.sock, buffer, sizeof(buffer), 0);

        if (received_count <= 0)
        {
            break;
        }

        if (!sendAll(server_sock, buffer, received_count))
        {
            break;
        }
    }

    shutdown(server_sock, SHUT_WR);

    while (true)
    {
        ssize_t received_count = recv(server_sock, buffer, sizeof(buffer), 0);

        if (received_count <= 0)
        {
            break;
        }

        if (!sendAll(c.sock, buffer, received_count))
        {
            break;
        }
    }

    close(server_sock);

    close(c.sock);
}

// =============================================================================================
// PART 3: THE TWO THREADS
// =============================================================================================

void acceptLoop(int listenSock)
{
    // Repeat: accept() a new client.
    //  If pendingClients.size() is already MAX_PENDING_CLIENTS: unlock, sendError "server busy, try again later",
    // else
    // Lock queueMutex.
    //   close the client, and go on.
    //   Otherwise enqueue the client socket.
    // Never wait for the server here, so new clients are always accepted quickly.
    // If accept() fails, print the error and keep going (do not end the thread).

    while (flag)
    {
        sockaddr_in client_socket_addr;

        socklen_t client_socket_addr_len = sizeof(client_socket_addr);

        int incoming_client_sock = accept(listenSock, (sockaddr *)&client_socket_addr, &client_socket_addr_len);

        if (incoming_client_sock < 0)
        {
            continue;
        }

        queueMutex.lock();

        if (pendingClients.size() >= MAX_PENDING_CLIENTS)
        {
            queueMutex.unlock();

            rejectClient(incoming_client_sock, "server busy, try again later");

            continue;
        }

        ClientInfo new_client_record;

        new_client_record.sock = incoming_client_sock;

        char client_ip_text[64];

        inet_ntop(AF_INET, &client_socket_addr.sin_addr, client_ip_text, sizeof(client_ip_text));

        new_client_record.address = string(client_ip_text);

        new_client_record.port = ntohs(client_socket_addr.sin_port);

        new_client_record.arrivedAt = time(nullptr);

        pendingClients.enqueue(new_client_record);

        queueMutex.unlock();
    }
}
void dispatcherLoop()
{
    // This runs in the main thread and never ends.
    // Repeat:
    //   Lock queueMutex, take ONE client out of the queue if there is one, unlock.
    //   If we got a client: call relayOneClient(). The next client is taken out of the queue only after this one is finished.

    while (flag)
    {
        bool has_pending_client = false;

        ClientInfo active_client_record;

        queueMutex.lock();

        if (!pendingClients.isEmpty())
        {
            active_client_record = pendingClients.dequeue();

            has_pending_client = true;
        }

        queueMutex.unlock();

        if (has_pending_client)
        {
            time_t current_timestamp = time(nullptr);

            if (current_timestamp - active_client_record.arrivedAt > 30)
            {
                close(active_client_record.sock);

                continue;
            }

            relayOneClient(active_client_record);
        }
        else
        {
            usleep(10000);
        }
    }
}

void handler(int sg) {
    (void)sg;
    flag = 0;
}
int main(int argc, char *argv[])
{

    signal(SIGINT, handler); // whenever ctrl+c will be pressed to exit the program the flag wll be set so insead of using infinite loop you can loop on flag to safely close things and exit
    // 2. listenSock = createListenSocket(LB_PORT).
    // 3. Start a thread that runs acceptLoop(listenSock) and detach it.
    // 4. Call dispatcherLoop().

    if (argc >= 2)
    {
        serverHost = argv[1];
    }

    if (argc >= 3)
    {
        serverPort = atoi(argv[2]);
    }

    if (argc >= 4)
    {
        lbPort = atoi(argv[3]);
    }

    int listen_socket_fd = createListenSocket(lbPort);

    thread client_acceptor_thread(acceptLoop, listen_socket_fd);

    client_acceptor_thread.detach();

    dispatcherLoop();

    close(listen_socket_fd);

    return 0;
}
