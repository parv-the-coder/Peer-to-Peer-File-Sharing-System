#include "client/downloader.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <iostream>
#include <queue>
#include <sstream>
#include <thread>
#include <tuple>

#include "common/hash.h"
#include "common/message.h"
#include "common/socket_io.h"

using namespace std;

namespace p2p {

void Downloader::download(const string &gid, const string &fname,
                          const string &destpath_in)
{
  string destpath = destpath_in;

  
  // checking if already downloading this file
  {
      lock_guard<mutex> lock(mtx_); 
      if (downloads_.find(fname) != downloads_.end() && downloads_[fname].is_active) 
      {
          cout << "File " << fname << " is already being downloaded.\n";
          return;
      }
  }

  // query tracker for file metadata and peers
  string tracker_cmd = "download_file " + tracker_.token() + " " + gid + " " + fname; 
  
  string r = tracker_.send(tracker_cmd); 
  if (r.rfind("FILE ", 0) != 0) 
  { 
      cout << r << endl; 
      return; 
  }

  // parsing file metadata
  stringstream s(r); 
  string token; 
  long long size = 0; 
  string fullhash;
  long long num_pieces = 0; 
  vector<string> piece_hashes; 
  string word; 
  while (s >> word) 
  {
      if (word == "SIZE") s >> size; 
      else if (word == "HASH") s >> fullhash; 
      else if (word == "PIECES") s >> num_pieces; 
      else if (word == "PIECE_HASHES") break; 
  }
  piece_hashes.resize(num_pieces); // resize
  for (long long i = 0; i < num_pieces; ++i)
  {
      s >> piece_hashes[i]; // get hashes
  } 

  // parses available peers
  size_t pos = r.find("\nPEERS\n"); // find peers
  vector<tuple<string,string,string>> peerlist; // peer list
  
  if (pos != string::npos) 
  {
      string peers_block = r.substr(pos + 7); // get block
      stringstream sp(peers_block); 
      string pname, pip, pport; 
      while (sp >> pname >> pip >> pport) 
      {
          peerlist.emplace_back(pname, pip, pport);
      }
  }

  if (peerlist.empty()) 
  { 
      cout << "No active peers available for " << fname << ".\n"; 
      return; 
  }

  if (destpath.back() != '/') 
  {
      destpath += "/";
  }

  string fullout = destpath + fname;
  FILE *outf = fopen(fullout.c_str(), "rb+"); 
  if (!outf) 
  {
      outf = fopen(fullout.c_str(), "wb+");
      if (!outf) 
      { 
          cout << "Failed to create output file: " << fullout << endl; 
          return; 
      }
  }
  // Pre-allocate file size for large files (>2GB support)
  if (size > 0) 
  {
      if (fseeko(outf, (off_t)(size - 1), SEEK_SET) != 0) 
      {
          cout << "Failed to seek to end of large file (size: " << size << " bytes)\n";
          fclose(outf);
          return;
      }
      if (fputc(0, outf) == EOF) 
      {
          cout << "Failed to write to end of large file\n";
          fclose(outf);
          return;
      }
  }
  if (fflush(outf) != 0) 
  {
      cout << "Failed to flush file allocation\n";
  }
  fclose(outf);

  vector<int> piece_status(num_pieces, 0); // status
  string statefile = fullout + ".downloading"; // state file
  FILE *statein = fopen(statefile.c_str(), "rb"); // open state
  if (statein) 
  {
      for (long long i = 0; i < num_pieces; ++i) 
      {
          int st = 0; 
          if (fread(&st, sizeof(int), 1, statein) == 1) piece_status[i] = st;
      }
      fclose(statein); 
  }

  // init download tracking
  DownloadInfo download_info;
  download_info.group_id = gid; 
  download_info.filename = fname; 
  download_info.dest_path = fullout; 
  download_info.total_size = size; 
  download_info.total_pieces = num_pieces; 
  download_info.completed_pieces = 0; 
  download_info.piece_status = piece_status; 
  download_info.piece_hashes = piece_hashes; 
  download_info.full_hash = fullhash; 
  download_info.is_active = true; 
  {
      lock_guard<mutex> lock(mtx_); 
      downloads_[fname] = download_info;
  }

  cout << "Starting download of " << fname << " (" << size << " bytes, " << num_pieces << " pieces) from " << peerlist.size() << " peers.\n";

  // thread-safe piece queue and status tracking
  queue<long long> pending_pieces; // queue
  for (long long i = 0; i < num_pieces; ++i)
  { 
      if (piece_status[i] != 2) 
      {
          pending_pieces.push(i);
      }
  }
  mutex queue_mtx; 
  const int MAX_RETRIES = 5; // max tries
  mutex state_mtx; 
  atomic<long long> completed_count(0); // completed

  // save state helper
  auto save_state = [&]() 
  {
      FILE *stateout = fopen(statefile.c_str(), "wb"); 
      if (!stateout) return; 
      for (long long i = 0; i < num_pieces; ++i)
      {
          fwrite(&piece_status[i], sizeof(int), 1, stateout);
      } 
      fclose(stateout); 
  };

  // worker function
  auto worker = [&](int worker_id) 
  {
      // Create peer order with worker-specific preference to avoid contention
      vector<int> peer_order(peerlist.size()); // peer order
      for (int i = 0; i < (int)peerlist.size(); ++i) 
      {
          peer_order[i] = i;
      }
      // start each worker from a different peer to distribute load
      rotate(peer_order.begin(), peer_order.begin() + (worker_id % peerlist.size()), peer_order.end()); // rotate
      
      while (true) 
      {
          long long piece_idx = -1; // piece index
          {
              lock_guard<mutex> lock(queue_mtx);
              if (!pending_pieces.empty()) {
                  piece_idx = pending_pieces.front(); 
                  pending_pieces.pop();
              }
          }
          if (piece_idx == -1) return; 

          {
              lock_guard<mutex> lock(mtx_);
              if (downloads_.find(fname) != downloads_.end()) 
              {
                  downloads_[fname].piece_status[piece_idx] = 1;
              }
          }
          piece_status[piece_idx] = 1; // set downloading
          save_state();

          int attempt = 0; // tries
          bool success = false; // success

          while (attempt < MAX_RETRIES && !success) 
          {
              for (int peer_idx : peer_order) 
              {
                  auto &p = peerlist[peer_idx]; 
                  string pip, pport, pname;
                  tie(pname, pip, pport) = p; 

                  int psock = socket(AF_INET, SOCK_STREAM, 0); 
                  if (psock < 0) continue; 

                  struct sockaddr_in addr; 
                  addr.sin_family = AF_INET; 
                  int peer_port = 0;
                  if (!p2p::parse_int(pport, peer_port) || peer_port <= 0 || peer_port > 65535)
                  {
                      close(psock);
                      continue;
                  }
                  addr.sin_port = htons(peer_port); 
                  if (inet_pton(AF_INET, pip.c_str(), &addr.sin_addr) <= 0) 
                  {
                      close(psock);
                      continue;
                  }

                  //  timeout for large pieces (512KB can take time on slow connections)
                  struct timeval tv = {30, 0}; // 30 second timeout
                  setsockopt(psock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv)); // set timeout
                  setsockopt(psock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv)); // set timeout

                  if (connect(psock, (struct sockaddr *)&addr, sizeof(addr)) < 0) 
                  {
                      close(psock);
                      continue;
                  }

                  string preq = "GET_PIECE " + fname + " " + to_string(piece_idx);
                  if (!send_framed(psock, preq))
                  {
                      close(psock);
                      continue;
                  }

                  string piece_payload;
                  if (!recv_framed(psock, piece_payload, kPieceSize))
                  {
                      close(psock);
                      continue;
                  }
                  close(psock);

                  size_t piece_size = piece_payload.size();
                  vector<char> buffer(piece_payload.begin(), piece_payload.end());

                  string recv_hex = p2p::sha1_hex(buffer.data(), piece_size); // get hash

                  if (recv_hex != piece_hashes[piece_idx]) {
                      cout << "[Piece " << piece_idx << "] Hash mismatch! Expected: " << piece_hashes[piece_idx] << ", Got: " << recv_hex << endl;
                      attempt++;
                      continue;
                  }

                  FILE *fw = fopen(fullout.c_str(), "rb+");
                  if (!fw) 
                  { 
                      cout << "[Piece " << piece_idx << "] Failed to open output file for writing\n";
                      attempt++; 
                      continue; 
                  }
                  
                  // Use fseeko for large file support (>2GB)
                  off_t offset = (off_t)piece_idx * kPieceSize; // offset
                  if (fseeko(fw, offset, SEEK_SET) != 0) 
                  {
                      cout << "[Piece " << piece_idx << "] Failed to seek to position " << offset << "\n";
                      fclose(fw);
                      attempt++;
                      continue;
                  }
                  
                  size_t written = fwrite(buffer.data(), 1, piece_size, fw); // write
                  if (written != piece_size) 
                  {
                      cout << "[Piece " << piece_idx << "] Failed to write complete piece. Expected: " << piece_size << ", Written: " << written << "\n";
                      fclose(fw);
                      attempt++;
                      continue;
                  }
                  
                  if (fflush(fw) != 0) 
                  {
                      cout << "[Piece " << piece_idx << "] Failed to flush data to disk\n";
                      fclose(fw);
                      attempt++;
                      continue;
                  }
                  fclose(fw);

                  {
                      lock_guard<mutex> lk(state_mtx); 
                      lock_guard<mutex> dl_lock(mtx_); 
                      if (downloads_.find(fname) != downloads_.end()) 
                      {
                          downloads_[fname].piece_status[piece_idx] = 2;
                          downloads_[fname].completed_pieces++;
                      }
                  }
                  piece_status[piece_idx] = 2; // set completed
                  save_state();

                  completed_count++; // add completed
                  long long progress_pct = (completed_count * 100) / num_pieces;
                  cout << "[Piece " << piece_idx << "] Downloaded successfully from " << pip << ":" << pport << " (" << completed_count << "/" << num_pieces << " = " << progress_pct << "%)\n";
                  
                  // report progress at milestones for large files
                  if (num_pieces > 100 && completed_count % (num_pieces / 10) == 0) 
                  {
                      cout << "*** Download Progress: " << progress_pct << "% complete ***\n";
                  }
                  success = true;
                  break;
              }
              if (!success) attempt++;
          }

          if (!success) {
              cout << "[Piece " << piece_idx << "] Failed after " << MAX_RETRIES << " attempts.\n";
              {
                  lock_guard<mutex> lock(mtx_);
                  if (downloads_.find(fname) != downloads_.end()) 
                  {
                      downloads_[fname].piece_status[piece_idx] = 3; // set failed
                  }
              }
              piece_status[piece_idx] = 3; // set failed
              save_state();
          }
      }
  };

  // limits workers for very large files to prevent memory/resource exhaustion
  int max_workers = (num_pieces > 1000) ? min(4, (int)peerlist.size()) : min((int)peerlist.size(), 8); // workers
  int num_workers = max_workers; // workers
  
  cout << "Using " << num_workers << " download workers for " << num_pieces << " pieces\n";
  
  vector<thread> dthreads; // threads
  for (int i=0;i<num_workers;i++)
  {
      dthreads.emplace_back(worker,i); // start threads
  } 
  for (auto &t:dthreads) 
  {
      if (t.joinable()) 
      {
          t.join(); // join
      }
  }

  // final verification
  bool all_completed = true; 
  {
      lock_guard<mutex> lock(mtx_);
      if (downloads_.find(fname) != downloads_.end()) 
      {
          for (long long i=0;i<num_pieces;i++) 
          {
              if (downloads_[fname].piece_status[i] != 2) 
              {
                  all_completed = false; // not completed
                  break;
              }
          }
      }
  }

  if (!all_completed) 
  {
      cout << "Download incomplete: some pieces failed.\n";
      {
          lock_guard<mutex> lock(mtx_);
          if (downloads_.find(fname) != downloads_.end()) 
          {
              downloads_[fname].is_active = false; // set inactive
          }
      }
      save_state();
      return;
  }

  // full file hash verification
  string dhash = sha1_file_hex(fullout); // get hash
  if (dhash == fullhash) 
  {
      cout << "[C] " << gid << " " << fname << " downloaded successfully.\n";
      
      // add downloaded file to uploaded_files so this peer can now serve it to others
      registry_.add(fname, fullout);
      
      // tell tracker that this peer now has the file so other peers can download
      string notify_cmd = "file_downloaded " + tracker_.token() + " " + gid + " " + fname; 
      string tracker_response = tracker_.send(notify_cmd); 
      
      {
          lock_guard<mutex> lock(mtx_); 
          if (downloads_.find(fname) != downloads_.end()) 
          {
              downloads_[fname].is_active = false; // set inactive
          }
      }
      remove(statefile.c_str());
  } 
  else 
  {
      cout << "Full-file hash mismatch! Expected " << fullhash << " got " << dhash << endl;
      {
          lock_guard<mutex> lock(mtx_); // lock
          if (downloads_.find(fname) != downloads_.end()) 
          {
              downloads_[fname].is_active = false; // set inactive
          }
      }
      save_state();
  }
}

string Downloader::report() const
{
  lock_guard<mutex> lock(mtx_);
  ostringstream out;
  bool any = false;
  out << "========== Downloads ==========\n";
  for (const auto &pair : downloads_)
  {
    const DownloadInfo &info = pair.second;
    bool completed = true;
    for (int status : info.piece_status)
    {
      if (status != 2) { completed = false; break; }
    }
    if (!info.is_active && !completed) continue;

    any = true;
    long long progress = info.total_pieces > 0
                             ? (info.completed_pieces * 100) / info.total_pieces
                             : 0;
    out << "File: " << info.filename << "\n";
    out << "  Group: " << info.group_id << "\n";
    out << "  Size: " << info.total_size << " bytes\n";
    out << "  Progress: " << info.completed_pieces << "/" << info.total_pieces
        << " pieces (" << progress << "%)\n";
    out << "  Status: ";

    int pending = 0, downloading = 0, done = 0, failed = 0;
    for (int status : info.piece_status)
    {
      if (status == 0) pending++;
      else if (status == 1) downloading++;
      else if (status == 2) done++;
      else if (status == 3) failed++;
    }
    if (completed)
    {
      out << "COMPLETED";
    }
    else
    {
      out << pending << " pending, " << downloading << " downloading, "
          << done << " completed, " << failed << " failed";
    }
    out << "\n----------------------------------------\n";
  }
  if (!any)
  {
    out << "No downloads found.\n";
  }
  return out.str();
}

} // namespace p2p
