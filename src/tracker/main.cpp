#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "common/message.h"
#include "common/socket_io.h"
#include "tracker/tracker_state.h"

using namespace std;
using p2p::recv_framed;
using p2p::Result;
using p2p::send_framed;
using p2p::split_args;
using p2p::TrackerState;

// The single shared instance of all tracker state. Every connection
// thread below drives this same object; TrackerState is responsible for
// its own locking.
static TrackerState g_state;

// Turns one parsed command into a reply. Argument-count validation lives
// here; everything that touches shared state goes through g_state, which
// takes the lock.
static string dispatch(const vector<string> &comds)
{
    const string &cmd = comds[0];
    const size_t n = comds.size();

    if (cmd == "create_user")
    {
        if (n != 3) return "-----Invalid Arguments-----";
        return g_state.create_user(comds[1], comds[2]).message;
    }
    if (cmd == "login")
    {
        if (n < 5) return "-----Invalid Arguments for login-----";
        return g_state.login(comds[1], comds[2], comds[3], comds[4]).message;
    }
    if (cmd == "logout")
    {
        if (n < 2) return "-----Invalid Arguments-----";
        return g_state.logout(comds[1]).message;
    }
    if (cmd == "create_group")
    {
        if (n < 3) return "-----Invalid Arguments-----";
        return g_state.create_group(comds[1], comds[2]).message;
    }
    if (cmd == "join_group")
    {
        if (n < 3) return "-----Invalid Arguments-----";
        return g_state.join_group(comds[1], comds[2]).message;
    }
    if (cmd == "leave_group")
    {
        if (n < 3) return "-----Invalid Arguments-----";
        return g_state.leave_group(comds[1], comds[2]).message;
    }
    if (cmd == "list_requests")
    {
        if (n < 3) return "-----Invalid Arguments-----";
        return g_state.list_requests(comds[1], comds[2]).message;
    }
    if (cmd == "accept_request")
    {
        if (n < 4) return "-----Invalid Arguments-----";
        return g_state.accept_request(comds[1], comds[2], comds[3]).message;
    }
    if (cmd == "list_groups")
    {
        return g_state.list_groups().message;
    }
    if (cmd == "upload_file")
    {
        // Needs gid, filename, user, size, hash and piece count before the
        // variable-length piece hash list starts at index 7. The previous
        // check only required 4 tokens but then read comds[4..6]
        // unconditionally, so a short upload_file read past the end.
        if (n < 7) return "-----Invalid Arguments for upload_file-----";
        long long fsize = atoll(comds[4].c_str());
        int num_pieces = stoi(comds[6]);
        vector<string> piece_hashes;
        for (size_t i = 7; i < n; ++i)
        {
            piece_hashes.push_back(comds[i]);
        }
        return g_state
            .upload_file(comds[1], comds[2], comds[3], fsize, comds[5],
                         num_pieces, piece_hashes)
            .message;
    }
    if (cmd == "list_files")
    {
        if (n < 3) return "-----Invalid Arguments-----";
        return g_state.list_files(comds[1], comds[2]).message;
    }
    if (cmd == "download_file")
    {
        if (n < 4) return "-----Invalid Arguments for download_file-----";
        return g_state.download_file(comds[1], comds[2], comds[3]).message;
    }
    if (cmd == "file_downloaded")
    {
        if (n != 4) return "-----Invalid Arguments for file_downloaded-----";
        return g_state.file_downloaded(comds[1], comds[2], comds[3]).message;
    }
    if (cmd == "stop_share")
    {
        if (n != 4) return "-----Invalid Arguments for stop_share-----";
        return g_state.stop_share(comds[1], comds[2], comds[3]).message;
    }
    return "Unrecognized command";
}

// Reads framed commands from one connected client until the connection
// drops, replying to each.
void managepeer(int peersocket)
{
    string disconnecting_user; // user to clean up after on disconnect

    while (true)
    {
        string buff;
        if (!recv_framed(peersocket, buff))
        {
            cout << "Connection closed or errored: " << peersocket << endl;
            g_state.handle_disconnect(disconnecting_user);
            close(peersocket);
            return;
        }

        cout << "Incoming command from socket " << peersocket << ": " << buff
             << endl;

        vector<string> comds = split_args(buff);
        if (comds.empty())
        {
            send_framed(peersocket, "Invalid command");
            continue;
        }

        // remember who this connection belongs to, for disconnect cleanup
        if ((comds[0] == "login" || comds[0] == "logout") && comds.size() > 1)
        {
            disconnecting_user = comds[1];
        }

        send_framed(peersocket, dispatch(comds));
    }
}

int main(int argc, char *argv[])
{
    if (argc != 3)
    {
        cout << "-----Invalid Arguments-----" << endl;
        return 1;
    }

    // reading tracker IP and port from config file
    FILE *filconfig = fopen(argv[1], "r"); // opening file
    if (!filconfig)
    {
        cout << "Failed to open tracker info file" << endl;
        return 1;
    }

    char ipbuf[128], portbuf[32]; // buffers
    if (fscanf(filconfig, "%127s %31s", ipbuf, portbuf) != 2)
    {
        cout << "Failed to read tracker info" << endl;
        fclose(filconfig);
        return 1;
    }

    fclose(filconfig); // closing file
    string serverip = ipbuf; // setting ip
    string serverport = portbuf; // setting port

    // TCP socket
    int serversock; // socket
    struct sockaddr_in serveradd; // address

    // creating server socket
    serversock = socket(AF_INET, SOCK_STREAM, 0); // socket
    if (serversock < 0)
    {
        perror("------- Error: Could not create socket -------");
        return 1;
    }

    // SO_REUSEADDR only: lets the tracker rebind a port still in TIME_WAIT
    // after a restart. Deliberately NOT SO_REUSEPORT, which would let a
    // second tracker bind the same port and have the kernel split incoming
    // clients between two processes with separate, diverging state.
    int choice = 1; // option
    if (setsockopt(serversock, SOL_SOCKET, SO_REUSEADDR, &choice, sizeof(choice)) != 0)
    {
        perror("------- Unable to configure socket options ------");
        return 1;
    }

    // binding server to IP and port
    serveradd.sin_family = AF_INET;
    serveradd.sin_addr.s_addr = INADDR_ANY; // any address
    int port = stoi(serverport); // port
    serveradd.sin_port = htons(port); // setting port

    if (bind(serversock, (struct sockaddr *)&serveradd, sizeof(serveradd)) < 0)
    {
        perror("------ Unable to bind socket ------");
        cout << "Is another tracker already running on port " << serverport << "?" << endl;
        return 1;
    }

    // listening for incoming connections
    if (listen(serversock, 20) < 0)
    {
        perror("------- Unable to start listening on socket ---------");
        return 1;
    }

    // server startup info and available commands
    cout << "\n=========================================\n";
    cout << "          TRACKER SERVER STARTED         \n";
    cout << "=========================================\n";
    cout << "Listening on IP: " << serverip << "  Port: " << serverport << endl;
    cout << "Tracker is now running...\n";
    cout << "-----------------------------------------\n";
    cout << "Available Tracker Commands (from console):\n";
    cout << "   quit   -> Stop the tracker server\n";
    cout << "-----------------------------------------\n\n";

    int incomsock; // incoming socket
    int length = sizeof(serveradd);

    // thread to handle console input
    thread exit_thread([]()
    {
        string inp;
        while (true)
        {
            getline(cin, inp);
            if (inp == "quit")
            {
                exit(0);
            }
        }
    });
    exit_thread.detach(); // detach

    // handle incoming client connections
    while (1)
    {
        if ((incomsock = accept(serversock, (struct sockaddr *)&serveradd, (socklen_t *)&length)) < 0)
        {
            cout << "------- Unable to accept incoming connection -------" << endl;
            continue;
        }
        cout << "******* Client accepted at socket: " << incomsock << " ******" << endl;
        // Detached: the accept loop never returns, so nothing would ever
        // join these. The previous code accumulated thread objects in a
        // vector it could not reach the join for, growing without bound.
        thread(managepeer, incomsock).detach();
    }

    return 0;
}
