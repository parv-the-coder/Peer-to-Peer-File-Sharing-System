#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <thread>
#include <fcntl.h>
#include <sys/types.h>
#include <stdio.h>
#include <iostream>
#include <vector>
#include <string>
#include <mutex>
#include <sstream>
#include <csignal>

#include "client/commands.h"
#include "client/downloader.h"
#include "client/peer_server.h"
#include "client/tracker_client.h"
#include "client/upload_registry.h"
#include "common/config.h"
#include "common/hash.h"
#include "common/message.h"
#include "common/socket_io.h"

using namespace std;
using p2p::split_args;

// The client's long-lived collaborators. Session state (who is logged in,
// and the token) lives in CommandProcessor rather than in globals, so the
// REPL and the web interface cannot disagree about it.
static p2p::TrackerClient tracker;
static p2p::UploadRegistry uploaded_files;

int main(int argc, char *argv[])
{
    // Belt and braces alongside MSG_NOSIGNAL in send_all: a peer hanging
    // up must never take this process down.
    signal(SIGPIPE, SIG_IGN);

    if (argc != 3) 
    { 
        cout << "----- Invalid Arguments ------" << endl; 
        return 0; 
    } 

    string hostip, hostport;
    int idx = 0;
    for (; argv[1][idx] != ':'; ++idx)
    {
        hostip.push_back(argv[1][idx]); // get ip
    }
    idx++;
    while (argv[1][idx] != '\0')
    {
        hostport.push_back(argv[1][idx]);
        idx++;
    }

    // Try each tracker in turn. The brief runs two so the system keeps
    // working while one is down; the client must therefore fail over
    // rather than give up on the first entry.
    vector<p2p::Endpoint> trackers = p2p::load_endpoints(argv[2]);
    if (trackers.empty())
    {
        cout << "No usable tracker entries in " << argv[2] << endl;
        return 1;
    }

    bool linked = false;
    for (size_t i = 0; i < trackers.size(); ++i)
    {
        if (tracker.connect_to(trackers[i].ip, trackers[i].port))
        {
            cout << "Connected to tracker " << (i + 1) << " at "
                 << trackers[i].ip << ":" << trackers[i].port << endl;
            linked = true;
            break;
        }
        cout << "Tracker " << (i + 1) << " at " << trackers[i].ip << ":"
             << trackers[i].port << " unreachable";
        cout << (i + 1 < trackers.size() ? ", trying the next one...\n" : "\n");
    }
    if (!linked)
    {
        cout << "------- No tracker reachable -------" << endl;
        return 1;
    }

    // Start serving pieces to other peers.
    p2p::PeerServer server(uploaded_files);
    if (!server.start(hostip, hostport))
    {
        cout << "------- Could not start peer server, exiting -------" << endl;
        tracker.disconnect();
        return 1;
    }
    p2p::Downloader downloader(tracker, uploaded_files);

    p2p::CommandProcessor processor(tracker, uploaded_files, downloader, hostport);
    cout << p2p::CommandProcessor::help();

    while (true)
    {
        cout << ">>> ";
        string line;
        if (!getline(cin, line))
        {
            break; // stdin closed
        }
        vector<string> args = split_args(line);
        if (args.empty())
        {
            cout << "-------- Unrecognized command. Enter a valid command. --------" << endl;
            continue;
        }
        if (args[0] == "exit")
        {
            break;
        }
        cout << processor.execute(args);
    }

    cout << "------- Exiting Client ---------" << endl;
    downloader.wait_all(); // let in-flight downloads finish
    tracker.disconnect();
    server.stop();
    return 0;
}
