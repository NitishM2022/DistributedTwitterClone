/*
 *
 * Copyright 2015, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <ctime>

#include <google/protobuf/timestamp.pb.h>
#include <google/protobuf/duration.pb.h>

#include <fstream>
#include <iostream>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <string>
#include <sstream>
#include <stdlib.h>
#include <unistd.h>

#include <fcntl.h>
#include <semaphore.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <unordered_set>

#include <google/protobuf/util/time_util.h>
#include <grpc++/grpc++.h>
#include <glog/logging.h>
#define log(severity, msg) \
  LOG(severity) << msg;    \
  google::FlushLogFiles(google::severity);

#include "sns.grpc.pb.h"
#include "coordinator.grpc.pb.h"

using google::protobuf::Duration;
using google::protobuf::Timestamp;
using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerReader;
using grpc::ServerReaderWriter;
using grpc::ServerWriter;
using grpc::Status;

using csce438::Confirmation;
using csce438::CoordService;
using csce438::ID;
using csce438::ListReply;
using csce438::Message;
using csce438::Reply;
using csce438::Request;
using csce438::ServerInfo;
using csce438::ServerList;
using csce438::SNSService;
using csce438::SynchService;

struct Client
{
  std::string username;
  bool connected = true;
  int following_file_size = 0;
  std::unordered_set<std::string> client_followers;
  std::unordered_set<std::string> client_following;
  ServerReaderWriter<Message, Message> *stream = 0;
  bool operator==(const Client &c1) const
  {
    return (username == c1.username);
  }
};

int clusterID = 1;
bool isMaster = false;
std::string clusterSubdirectory = "2"; // default to slave

// map that stores the number of lines in each file
std::mutex ffl_mutex;
std::unordered_map<std::string, int> followingFileLines;

// map that stores every client that has been created
std::mutex db_mutex;
std::unordered_map<std::string, Client *> client_db;

std::atomic<bool> alive = true;

Client *getClient(const std::string &);
std::vector<Message> getLastNPosts(std::string, int = -1);
void appendTo(const std::string &, const std::string &, const std::string &, const std::string &);

class SNSServiceImpl final : public SNSService::Service
{

  Status List(ServerContext *context, const Request *request, ListReply *list_reply) override
  {
    std::string u = request->username();

    db_mutex.lock();
    // Add all users to the list reply
    for (const auto &client : client_db)
      list_reply->add_all_users(client.second->username);
    db_mutex.unlock();

    Client *c = getClient(u);

    // Add followers to the list reply
    if (c != nullptr)
    {
      db_mutex.lock();
      for (const auto &follower : c->client_followers)
      {
        Client *f = getClient(follower);
        if (f != nullptr)
          list_reply->add_followers(f->username);
      }
      db_mutex.unlock();
    }

    return Status::OK;
  }

  Status Follow(ServerContext *context, const Request *request, Reply *reply) override
  {
    std::string u1 = request->username();
    std::string u2 = request->arguments(0);

    db_mutex.lock();
    if (u1 == u2)
    {
      reply->set_msg("cannot follow yourself");
      return Status::OK;
    }

    Client *c1 = getClient(u1);
    Client *c2 = getClient(u2);
    if (c2 == nullptr)
    {
      reply->set_msg("user to follow does not exist");
      return Status::OK;
    }

    if (c2->client_followers.find(u1) != c2->client_followers.end())
    {
      reply->set_msg("already following");
      db_mutex.unlock();
      return Status::OK;
    }

    // update client db and write to the file
    std::string file = "cluster_" + std::to_string(clusterID) + "/" + clusterSubdirectory + "/" + c1->username + "_follow_list.txt";
    std::string semName = "/" + std::to_string(clusterID) + "_" + clusterSubdirectory + "_" + c1->username + "_follow_list.txt";
    sem_t *fileSem = sem_open(semName.c_str(), O_CREAT, 0666, 1);

    sem_wait(fileSem);
    std::ofstream followingStream(file, std::ios::app | std::ios::out | std::ios::in);
    followingStream << c2->username << std::endl;
    followingStream.close();
    sem_post(fileSem);
    sem_close(fileSem);

    c1->client_following.insert(c2->username);

    file = "cluster_" + std::to_string(clusterID) + "/" + clusterSubdirectory + "/" + c2->username + "_followers.txt";
    semName = "/" + std::to_string(clusterID) + "_" + clusterSubdirectory + "_" + c2->username + "_followers.txt";
    fileSem = sem_open(semName.c_str(), O_CREAT, 0666, 1);

    sem_wait(fileSem);
    std::ofstream followerStream(file, std::ios::app | std::ios::out | std::ios::in);
    followerStream << c1->username << std::endl;
    followerStream.close();
    sem_post(fileSem);
    sem_close(fileSem);

    c2->client_followers.insert(c1->username);
    reply->set_msg("follow successful");
    db_mutex.unlock();

    return Status::OK;
  }

  Status UnFollow(ServerContext *context, const Request *request, Reply *reply) override
  {
    // std::string u1 = request->username();
    // std::string u2 = request->arguments(0);
    // Client *c1 = getClient(u1);
    // Client *c2 = getClient(u2);

    // auto it = std::find(c1->client_following.begin(), c1->client_following.end(), c2);
    // if (it == c1->client_following.end())
    // {
    //   reply->set_msg("not following");
    //   return Status::OK;
    // }

    // c1->client_following.erase(it);
    // c2->client_followers.erase(std::remove(c2->client_followers.begin(), c2->client_followers.end(), c1), c2->client_followers.end());
    // reply->set_msg("unfollow successful");

    return Status::OK;
  }

  Status Login(ServerContext *context, const Request *request, Reply *reply) override
  {
    Client *c = getClient(request->username());
    if (c != nullptr)
    {
      if (c->connected)
      {
        reply->set_msg("user already connected");
      }
      else
      {
        c->connected = true;
        reply->set_msg("login succeeded");
      }
    }
    else
    {
      Client *new_client = new Client();
      new_client->username = request->username();
      new_client->connected = true;

      db_mutex.lock();
      client_db[new_client->username] = new_client;
      db_mutex.unlock();

      reply->set_msg("login succeeded: new user created");

      // must add the new user to the all_users.txt file
      std::string usersFile = "./cluster_" + std::to_string(clusterID) + "/" + clusterSubdirectory + "/all_users.txt";
      std::string semName = "/" + std::to_string(clusterID) + "_" + clusterSubdirectory + "_all_users.txt";
      sem_t *fileSem = sem_open(semName.c_str(), O_CREAT, 0666, 1);

      sem_wait(fileSem);

      std::ofstream userStream(usersFile, std::ios::app | std::ios::out | std::ios::in);
      userStream << new_client->username << std::endl;

      sem_post(fileSem);
      sem_close(fileSem);
    }

    return Status::OK;
  }

  Status Timeline(ServerContext *context, ServerReaderWriter<Message, Message> *stream) override
  {
    Message m;
    Client *curr = nullptr;
    std::string u;

    // Initialization block (executed only once)
    if (stream->Read(&m))
    {
      u = m.username();
      std::vector<Message> last_posts = getLastNPosts(u, 20);

      for (const auto &post : last_posts)
      {
        stream->Write(post);
      }

      Message done;
      done.set_msg("DONE");
      stream->Write(done);
    }

    // thread to monitor u_following.txt file
    std::thread following([&]()
                          {
      while (alive.load()) {
        // -1 indicates get all posts after followingFileLines
        std::vector<Message> newPosts = getLastNPosts(u, -1);
        for (const auto &post : newPosts)
          stream->Write(post);

        std::this_thread::sleep_for(std::chrono::seconds(5));
      } });

    // update client timeline file
    while (stream->Read(&m))
    {
      std::time_t time = m.timestamp().seconds();
      std::tm *ltime = std::localtime(&time);
      std::stringstream ss;
      ss << std::put_time(ltime, "%Y-%m-%d %H:%M:%S");
      std::string time_str = ss.str();

      appendTo(u + "_timeline.txt", time_str, u, m.msg());
    }

    following.join();
    return Status::OK;
  }
};

Client *getClient(const std::string &username)
{
  db_mutex.lock();
  auto it = client_db.find(username);
  if (it != client_db.end())
    return it->second;
  db_mutex.unlock();
  log(ERROR, "Client not found: " + username);

  return nullptr;
}

std::vector<Message> getLastNPosts(std::string u, int n)
{
  std::vector<Message> posts;

  // Get last n posts from the user's following file
  std::string followingFile = "cluster_" + std::to_string(clusterID) + "/" + clusterSubdirectory + "/" + u + "_following.txt";
  std::string semName = "/" + std::to_string(clusterID) + "_" + clusterSubdirectory + "_" + u + "_following.txt";
  sem_t *fileSem = sem_open(semName.c_str(), O_CREAT, 0666, 1);

  sem_wait(fileSem);
  std::ifstream infile(followingFile);

  if (!infile.is_open())
    return posts;

  std::vector<std::string> lines;
  std::string line;

  while (std::getline(infile, line))
    lines.push_back(line);

  infile.close();
  sem_post(fileSem);
  sem_close(fileSem);

  ffl_mutex.lock();
  if (followingFileLines.find(u) == followingFileLines.end())
    followingFileLines[u] = 0;
  int start = followingFileLines[u];
  ffl_mutex.unlock();

  if (n != -1)
    start = std::max(start, static_cast<int>(lines.size()) - (n * 4));

  for (int i = start; i <= lines.size() - 4; i += 4)
  {
    if (lines[i].substr(0, 2) != "T " ||
        lines[i + 1].substr(0, 2) != "U " ||
        lines[i + 2].substr(0, 2) != "W " ||
        !lines[i + 3].empty())
    {
      continue;
    }

    std::string timestamp_str = lines[i].substr(2);
    std::string username = lines[i + 1].substr(2);
    std::string message = lines[i + 2].substr(2);

    std::tm tm{};
    std::time_t post_time = 0;

    if (strptime(timestamp_str.c_str(), "%Y-%m-%d %H:%M:%S", &tm))
    {
      post_time = mktime(&tm);
    }
    else
    {
      log(ERROR, "Error parsing time string: " + timestamp_str);
      continue;
    }

    Message msg;
    google::protobuf::Timestamp *timestamp = msg.mutable_timestamp();
    timestamp->set_seconds(post_time);
    msg.set_username(username);
    msg.set_msg(message);

    posts.push_back(std::move(msg));
  }

  ffl_mutex.lock();
  followingFileLines[u] = lines.size();
  ffl_mutex.unlock();

  return posts;
}

void appendTo(const std::string &filename, const std::string &timestamp_str, const std::string &username, const std::string &message)
{
  std::ofstream outfile(filename, std::ios_base::app);
  if (outfile.is_open())
  {
    outfile << "T " << timestamp_str << "\n";
    outfile << "U " << username << "\n";
    outfile << "W " << message << "\n";
    outfile.close();
  }
  else
  {
    log(ERROR, "Error opening file: " + filename);
  }
}

void RunServer(int clusterID, int serverId, std::string port_no, std::string coordinatorIP, std::string coordinatorPort)
{
  std::string server_address = "0.0.0.0:" + port_no;
  SNSServiceImpl service;

  ServerBuilder builder;
  builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);
  std::unique_ptr<Server> server(builder.BuildAndStart());
  std::cout << "Server listening on " << server_address << std::endl;
  log(INFO, "Server listening on " + server_address);

  // Make directory for cluster
  std::string masterDir = "cluster_" + std::to_string(clusterID) + "/1";
  std::string slaveDir = "cluster_" + std::to_string(clusterID) + "/2";
  if (!std::filesystem::exists(masterDir)) {
      std::filesystem::create_directories(masterDir); 
  }
  if (!std::filesystem::exists(slaveDir)) {
      std::filesystem::create_directories(slaveDir); 
  }

  std::thread heartbeat([=]()
                        {
    bool registered = false;
    std::string coordinator_address = coordinatorIP + ":" + coordinatorPort;
    std::unique_ptr<csce438::CoordService::Stub> stub = csce438::CoordService::NewStub(grpc::CreateChannel(coordinator_address, grpc::InsecureChannelCredentials()));
    grpc::ClientContext context;

    csce438::ServerInfo request;
    request.set_serverid(serverId);
    request.set_hostname("0.0.0.0");
    request.set_port(port_no);
    request.set_type("server");
    request.set_clusterid(clusterID);
    csce438::Confirmation reply;

    while (alive.load()) {
      if (!registered){
        context.AddMetadata("clusterid", std::to_string(clusterID));
      }

      grpc::Status status = stub->Heartbeat(&context, request, &reply);

      if (status.ok()) {
        log(INFO, "Server " + std::to_string(request.serverid()) + " sent Heartbeat successfully.");
        isMaster = reply.ismaster();
        if (isMaster){
          log(INFO, "Server " + std::to_string(request.serverid()) + " is a master.");
          clusterSubdirectory= "1";
        }
        else{
          log(INFO, "Server " + std::to_string(request.serverid()) + " is a slave.");
          clusterSubdirectory= "2";
        }
        if (!registered)
          registered = true;
      } else {
        log(ERROR, "Heartbeat failed");
      }

      std::this_thread::sleep_for(std::chrono::seconds(5));
    } });

  // update client db every 5 seconds
  std::thread db([&]()
                 {
    while (alive.load()) {
      // add new clients to the client db
      std::string usersFile = "cluster_" + std::to_string(clusterID) + "/" + clusterSubdirectory + "/all_users.txt";
      std::string semName = "/" + std::to_string(clusterID) + "_" + clusterSubdirectory + "_all_users.txt";
      sem_t *fileSem = sem_open(semName.c_str(), O_CREAT, 0666, 1);

      sem_wait(fileSem);
      std::ifstream userStream(usersFile);
      if (userStream.is_open()) {
        std::string user;
        while (userStream >> user) {
          Client *c = getClient(user);

          if (c == nullptr) {
            Client *new_client = new Client();
            new_client->username = user;

            db_mutex.lock();
            client_db[new_client->username] = new_client;
            db_mutex.unlock();
          }
        }
        userStream.close();
        sem_post(fileSem);
        sem_close(fileSem);
        log(INFO, "Client db updated successfully.");
      } else {
        sem_post(fileSem);
        sem_close(fileSem);
        log(ERROR, "Error opening user file: " + usersFile);
      }
      
      std::this_thread::sleep_for(std::chrono::seconds(5));
    } });

  server->Wait();
  
  alive.store(false);
  // Wait for threads to finish
  heartbeat.join();
  db.join();
}

int main(int argc, char **argv)
{

  clusterID = 1;
  int serverId = 1;
  std::string coordinatorIP = "localhost";
  std::string coordinatorPort = "3010";
  std::string port = "3010";

  int opt = 0;
  while ((opt = getopt(argc, argv, "c:s:h:k:p:")) != -1)
  {
    switch (opt)
    {
    case 'c':
      clusterID = atoi(optarg);
      break;
    case 's':
      serverId = atoi(optarg);
      break;
    case 'h':
      coordinatorIP = optarg;
      break;
    case 'k':
      coordinatorPort = optarg;
      break;
    case 'p':
      port = optarg;
      break;
    default:
      std::cout << "Invalid Command Line Argument\n";
      log(ERROR, "Invalid Command Line Argument");
      return 1;
    }
  }

  std::string log_file_name = std::string("server-") + port;
  google::InitGoogleLogging(log_file_name.c_str());
  log(INFO, "Logging Initialized. Server starting...");
  RunServer(clusterID, serverId, port, coordinatorIP, coordinatorPort);

  return 0;
}
