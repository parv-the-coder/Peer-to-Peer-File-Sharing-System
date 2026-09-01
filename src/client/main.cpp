#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <thread>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <iostream>
#include <vector>
#include <unordered_map>
#include <string>
#include <queue>
#include <condition_variable>
#include <mutex>
#include <sstream>
#include <functional>
#include <algorithm>
#include <openssl/evp.h>
#include <atomic>
#include <csignal>

#include "client/downloader.h"
#include "client/peer_server.h"
#include "client/tracker_client.h"
#include "client/upload_registry.h"
#include "common/config.h"
#include "common/hash.h"
#include "common/message.h"
#include "common/socket_io.h"

using namespace std;
using p2p::recv_framed;
using p2p::send_framed;
using p2p::split_args;

static const size_t PIECE_SIZE = p2p::kPieceSize; // 512KB piece size

// globals
string peername; // name of peer

bool connected; // is connected
p2p::TrackerClient tracker; // connection to the tracker + session token
p2p::UploadRegistry uploaded_files; // fname -> local path, mutex-guarded
p2p::PeerServer *peer_server = nullptr; // serves GET_PIECE to other peers

void displaycomds() 
{
    cout << "\n==================== Available Commands ====================\n";
    cout << "create_user <username> <password>\n";
    cout << "login <username> <password>\n";
    cout << "logout\n";
    cout << "create_group <groupid>\n";
    cout << "join_group <groupid>\n";
    cout << "leave_group <groupid>\n";
    cout << "list_requests <groupid>\n";
    cout << "accept_request <groupid> <username>\n";
    cout << "reject_request <groupid> <username>\n";
    cout << "list_groups\n";
    cout << "list_files <groupid>\n";
    cout << "upload_file <groupid> <filepath>\n";
    cout << "download_file <groupid> <filename> <dest_path>\n";
    cout << "stop_share <groupid> <filename>\n";
    cout << "show_downloads\n";
    cout << "commands\n";
    cout << "exit\n";
    cout << "============================================================\n\n";
}

// set peername, token and connected
void login_local(const string &str, const string &token)
{
    peername = str; tracker.set_token(token); connected = true;
}

// reset peername, token and connected
void logout_local()
{
    peername = ""; tracker.clear_token(); connected = false;
}

void logincheck(function<void()> action) 
{
    // check login
    if (!connected) 
    { 
        cout << "------- You must log in first. ---------" << endl; 
        return; 
    } 
    action();
}

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
    logout_local(); // logout

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
    peer_server = &server;
    p2p::Downloader downloader(tracker, uploaded_files);
    displaycomds(); // show commands

    while (1) 
    {
        cout << ">>> "; 
        string comd; 
        getline(cin, comd); 
        vector<string> cmds; 
        string token; 
        stringstream ss(comd); 
        while (ss >> token)
        {
            cmds.push_back(token); 
        } 
        int length = cmds.size(); 
        if (length == 0) 
        { 
            cout << "-------- Unrecognized command. Enter a valid command. --------" << endl; 
            continue; 
        }
            
        unordered_map<string, function<void()>> cmdMap;
        // stop_share command
        cmdMap["stop_share"] = [&]() 
        {
            logincheck([&]() 
            {
                if (length != 3) 
                { 
                    cout << "Usage: stop_share <groupid> <filename>\n"; 
                    return; 
                }
                string gid = cmds[1], fname = cmds[2];
                // Remove from local uploaded_files
                if (uploaded_files.contains(fname))
                {
                    uploaded_files.remove(fname);
                    cout << "Stopped sharing file: " << fname << " in group " << gid << endl;
                } 
                else 
                {
                    cout << "File " << fname << " is not being shared by you." << endl;
                }
                // telling tracker to remove this peer as seeder
                string msg = "stop_share " + tracker.token() + " " + gid + " " + fname; 
                string resp = tracker.send(msg); 
                cout << resp << endl;
            });
        };

        cmdMap["create_user"] = [&]() 
        {
            if (length != 3) 
            { 
                cout << "Usage: create_user <user> <pass>\n"; 
                return; 
            }
            string msg = "create_user " + cmds[1] + " " + cmds[2];
            cout << tracker.send(msg) << endl; 
        };

        cmdMap["login"] = [&]() 
        {
            if (length != 3) 
            { 
                cout << "Usage: login <user> <pass>\n"; 
                return; 
            }
            if (connected) 
            { 
                cout << "-------- User session already active --------" << endl; 
                return; 
            }
            // Only the listening port is sent: the tracker takes our IP
            // from the socket rather than trusting what we claim.
            string msg = "login " + cmds[1] + " " + cmds[2] + " " + hostport;
            string r = tracker.send(msg);
            if (r.rfind("OK ", 0) == 0)
            {
                logout_local();
                login_local(cmds[1], r.substr(3));
                cout << "********* You are now logged in *********" << endl;
            }
            else cout << r << endl;
        };

        cmdMap["logout"] = [&]() 
        {
            logincheck([&]() 
            {
                string r = tracker.send("logout " + tracker.token());
                cout << r << endl; 
                logout_local(); 
            });
        };

        cmdMap["create_group"] = [&]() 
        {
            logincheck([&]() 
            {
                if (length != 2) 
                { 
                    cout << "Usage: create_group <groupid>\n"; 
                    return; 
                }
                cout << tracker.send("create_group " + tracker.token() + " " + cmds[1]) << endl; 
            });
        };

        cmdMap["join_group"] = [&]() 
        {
            logincheck([&]() 
            {
                if (length != 2) 
                { 
                    cout << "Usage: join_group <groupid>\n"; 
                    return; 
                }
                cout << tracker.send("join_group " + tracker.token() + " " + cmds[1]) << endl; 
            });
        };

        cmdMap["leave_group"] = [&]() 
        {
            logincheck([&]() 
            {
                if (length != 2) 
                { 
                    cout << "Usage: leave_group <groupid>\n"; 
                    return; 
                }
                cout << tracker.send("leave_group " + tracker.token() + " " + cmds[1]) << endl; 
            });
        };

        cmdMap["list_requests"] = [&]() 
        {
            logincheck([&]() 
            {
                if (length != 2) 
                { 
                    cout << "Usage: list_requests <groupid>\n"; 
                    return; 
                }
                cout << tracker.send("list_requests " + tracker.token() + " " + cmds[1]) << endl; 
            });
        };

        cmdMap["accept_request"] = [&]() 
        {
            logincheck([&]() 
            {
                if (length != 3) 
                { 
                    cout << "Usage: accept_request <groupid> <user>\n"; 
                    return; 
                } 
                cout << tracker.send("accept_request " + tracker.token() + " " + cmds[1] + " " + cmds[2]) << endl; 
            });
        };

        cmdMap["reject_request"] = [&]() 
        {
            logincheck([&]() 
            {
                if (length != 3) 
                { 
                    cout << "Usage: reject_request <groupid> <user>\n"; 
                    return; 
                } 
                cout << tracker.send("reject_request " + tracker.token() + " " + cmds[1] + " " + cmds[2]) << endl; 
            });
        };

        cmdMap["list_groups"] = [&]() 
        {
            logincheck([&]() 
            { 
                cout << tracker.send("list_groups " + tracker.token()) << endl; 
            }); 
        };

        cmdMap["list_files"] = [&]() 
        {
            logincheck([&]() 
            {
                if (length != 2) 
                { 
                    cout << "Usage: list_files <groupid>\n"; 
                    return; 
                }
                cout << tracker.send("list_files " + tracker.token() + " " + cmds[1]) << endl; 
            });
        };

        // upload_file: allow spaces in filepath by recombining tokens
        cmdMap["upload_file"] = [&]() 
        {
            logincheck([&]() 
            {
                if (length < 3) 
                { 
                    cout << "Usage: upload_file <groupid> <filepath>\n"; 
                    return; 
                }
                string gid = cmds[1]; // group id
                string fpath; // file path
                for (int i = 2; i < length; ++i) 
                {
                    if (i > 2) fpath += " "; // add space
                    fpath += cmds[i]; // add token
                }
                int fd = open(fpath.c_str(), O_RDONLY);
                if (fd < 0) 
                { 
                    cout << "File not found: " << fpath << endl; 
                    return; 
                } 
                off_t fsize = lseek(fd, 0, SEEK_END);
                close(fd);
                if (fsize == 0) 
                { 
                    cout << "Cannot upload empty file: " << fpath << endl; 
                    return; 
                }
                // compute piece hashes and full hash
                long long num_pieces = 0; // pieces
                vector<string> piece_hashes = p2p::sha1_file_pieces(fpath, num_pieces); // get hashes
                string fullhash = p2p::sha1_file_hex(fpath); // get full hash
                size_t pos = fpath.find_last_of("/"); // find last /
                string fname = (pos == string::npos) ? fpath : fpath.substr(pos + 1); // get file name
                // store file locally so peer server can serve pieces
                uploaded_files.add(fname, fpath);

                string cmd = "upload_file " + tracker.token() + " " + gid + " " + fname + " " + to_string(fsize) + " " + fullhash + " " + to_string(num_pieces);
                for (auto &h : piece_hashes) cmd += " " + h; // add hashes
                string r = tracker.send(cmd);
                cout << r << endl;
            });
        };

        // download_file command
        cmdMap["download_file"] = [&]()
        {
            logincheck([&]()
            {
                if (length < 4)
                {
                    cout << "Usage: download_file <groupid> <filename> <dest_path>\n";
                    return;
                }
                // Returns immediately; the transfer runs on its own thread so
                // several files can download at once and the prompt stays live.
                if (downloader.start(cmds[1], cmds[2], cmds[3]))
                {
                    cout << "Download started in background. Use show_downloads for progress.\n";
                }
            });
        };

        cmdMap["show_downloads"] = [&]()
        {
            logincheck([&]()
            {
                cout << downloader.report();
            });
        };

        // handle exit
        if (cmds[0] == "exit") {
            cout << "------- Exiting Client ---------" << endl;
            downloader.wait_all(); // let in-flight downloads finish
            tracker.disconnect();
            server.stop(); // joins the accept loop and worker pool
            return 0;
        } 
        else if (cmds[0] == "commands") 
        {
            displaycomds(); // show commands
        }
        else if (cmdMap.find(cmds[0]) != cmdMap.end()) 
        {
           
            cmdMap[cmds[0]]();
        }
        else 
        {
            for (auto& pair : cmdMap)
            {
                cout << pair.first << " "<<endl;
            } 
            cout << endl;
            cout << "------- Invalid Command --------" << endl; // error
        }
    }
    
    server.stop();
    return 0;
}
