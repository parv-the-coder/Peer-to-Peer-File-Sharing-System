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

#include "common/hash.h"
#include "common/message.h"
#include "common/socket_io.h"
#include "tracker/session_manager.h"
#include "tracker/tracker_state.h"

using namespace std;
using p2p::recv_framed;
using p2p::Result;
using p2p::send_framed;
using p2p::split_args;
using p2p::SessionManager;
using p2p::TrackerState;

// The single shared instance of all tracker state. Every connection
// thread below drives this same object; TrackerState is responsible for
// its own locking.
static TrackerState g_state;
// Session tokens, with their own independent lock.
static SessionManager g_sessions;

// Reads the peer's address off the accepted socket. This is the
// authoritative source for a peer's IP: the previous protocol let the
// client send whatever IP it liked at login, so a client could advertise
// someone else's address and have other peers directed there.
static string socket_peer_ip(int fd)
{
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    if (getpeername(fd, (struct sockaddr *)&addr, &len) != 0)
    {
        return "";
    }
    char buf[INET_ADDRSTRLEN] = {0};
    if (!inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof(buf)))
    {
        return "";
    }
    return string(buf);
}

// Turns one parsed command into a reply.
//
// Identity comes from the session token, never from a username argument.
// Previously every privileged command took a bare <username> string and
// the tracker simply believed it, so any connected client could act as
// any user by naming them.
static string dispatch(const vector<string> &comds, const string &peer_ip)
{
    const string &cmd = comds[0];
    const size_t n = comds.size();

    static const string kAuthErr =
        "ERROR: AUTH_REQUIRED - session invalid or expired, please log in again";

    // --- unauthenticated commands ---

    if (cmd == "create_user")
    {
        if (n != 3) return "-----Invalid Arguments-----";
        return g_state.create_user(comds[1], comds[2]).message;
    }
    if (cmd == "login")
    {
        // login <username> <password> <listen_port>
        // The peer's IP is taken from the socket; only the port it listens
        // on is client-supplied, since that cannot be derived from an
        // inbound connection.
        if (n != 4) return "-----Invalid Arguments for login-----";
        int listen_port = 0;
        if (!p2p::parse_int(comds[3], listen_port) || listen_port <= 0 ||
            listen_port > 65535)
        {
            return "-----Invalid Arguments for login-----";
        }
        Result r = g_state.login(comds[1], comds[2], peer_ip, comds[3]);
        if (!r.ok) return r.message;
        string token = g_sessions.create(comds[1], peer_ip, comds[3]);
        if (token.empty())
        {
            return "------ Login failed: server entropy unavailable ------";
        }
        // "OK <token>" -- the client stores the token and attaches it to
        // every subsequent command.
        return "OK " + token;
    }

    // --- everything below requires a valid session ---

    if (n < 2) return kAuthErr;
    const string &token = comds[1];

    if (cmd == "logout")
    {
        string user = g_sessions.destroy(token);
        if (user.empty()) return kAuthErr;
        return g_state.logout(user).message;
    }

    string user;
    if (!g_sessions.username_for(token, user)) return kAuthErr;

    if (cmd == "create_group")
    {
        if (n != 3) return "-----Invalid Arguments-----";
        return g_state.create_group(comds[2], user).message;
    }
    if (cmd == "join_group")
    {
        if (n != 3) return "-----Invalid Arguments-----";
        return g_state.join_group(comds[2], user).message;
    }
    if (cmd == "leave_group")
    {
        if (n != 3) return "-----Invalid Arguments-----";
        return g_state.leave_group(comds[2], user).message;
    }
    if (cmd == "list_requests")
    {
        if (n != 3) return "-----Invalid Arguments-----";
        return g_state.list_requests(comds[2], user).message;
    }
    if (cmd == "accept_request")
    {
        if (n != 4) return "-----Invalid Arguments-----";
        return g_state.accept_request(comds[2], comds[3], user).message;
    }
    if (cmd == "list_groups")
    {
        return g_state.list_groups().message;
    }
    if (cmd == "upload_file")
    {
        // upload_file <token> <gid> <fname> <size> <hash> <num_pieces> <hashes...>
        if (n < 7) return "-----Invalid Arguments for upload_file-----";
        long long fsize = 0;
        int num_pieces = 0;
        if (!p2p::parse_ll(comds[4], fsize) || !p2p::parse_int(comds[6], num_pieces))
        {
            return "-----Invalid Arguments for upload_file-----";
        }
        if (fsize < 0 || num_pieces < 0)
        {
            return "-----Invalid Arguments for upload_file-----";
        }
        // The piece count must agree with the size, and the hash list must
        // be exactly that long. Without this an unvalidated num_pieces is
        // an allocation bomb: the tracker would resize a vector to whatever
        // the client claimed.
        long long expected =
            (fsize + (long long)p2p::kPieceSize - 1) / (long long)p2p::kPieceSize;
        if ((long long)num_pieces != expected || n != (size_t)num_pieces + 7)
        {
            return "-----Invalid Arguments for upload_file-----";
        }
        vector<string> piece_hashes;
        for (size_t i = 7; i < n; ++i)
        {
            piece_hashes.push_back(comds[i]);
        }
        return g_state
            .upload_file(comds[2], comds[3], user, fsize, comds[5], num_pieces,
                         piece_hashes)
            .message;
    }
    if (cmd == "list_files")
    {
        if (n != 3) return "-----Invalid Arguments-----";
        return g_state.list_files(comds[2], user).message;
    }
    if (cmd == "download_file")
    {
        // download_file <token> <gid> <fname>
        if (n != 4) return "-----Invalid Arguments for download_file-----";
        return g_state.download_file(comds[2], comds[3], user).message;
    }
    if (cmd == "file_downloaded")
    {
        if (n != 4) return "-----Invalid Arguments for file_downloaded-----";
        return g_state.file_downloaded(comds[2], comds[3], user).message;
    }
    if (cmd == "stop_share")
    {
        if (n != 4) return "-----Invalid Arguments for stop_share-----";
        return g_state.stop_share(comds[2], comds[3], user).message;
    }
    return "Unrecognized command";
}

// Reads framed commands from one connected client until the connection
// drops, replying to each.
void managepeer(int peersocket)
{
    const string peer_ip = socket_peer_ip(peersocket);
    if (peer_ip.empty())
    {
        cout << "Could not determine peer address, dropping socket "
             << peersocket << endl;
        close(peersocket);
        return;
    }

    string session_user; // who this connection authenticated as

    while (true)
    {
        string buff;
        if (!recv_framed(peersocket, buff))
        {
            cout << "Connection closed or errored: " << peersocket << endl;
            g_state.handle_disconnect(session_user);
            close(peersocket);
            return;
        }

        vector<string> comds = split_args(buff);
        if (comds.empty())
        {
            send_framed(peersocket, "Invalid command");
            continue;
        }

        // Log the verb only. The full line carries passwords on login and
        // session tokens on everything else, and this goes to the console.
        cout << "Command from socket " << peersocket << ": " << comds[0] << endl;

        string reply = dispatch(comds, peer_ip);

        // Track the authenticated user so a dropped connection can be
        // cleaned up. Set only after the tracker itself accepted the login.
        if (comds[0] == "login" && reply.rfind("OK ", 0) == 0)
        {
            session_user = comds[1];
        }
        else if (comds[0] == "logout")
        {
            session_user.clear();
        }

        send_framed(peersocket, reply);
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
    int port = 0;
    if (!p2p::parse_int(serverport, port) || port <= 0 || port > 65535)
    {
        cout << "Invalid port in tracker info file: " << serverport << endl;
        return 1;
    }
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
