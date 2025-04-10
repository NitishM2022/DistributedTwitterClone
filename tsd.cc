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
  bool connected = false;
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

std::unique_ptr<csce438::CoordService::Stub> coordinator_stub_;

std::atomic<bool> alive = true;

Client *getClient(const std::string &);
int getClusterID(const std::string &);
std::vector<Message> getLastNPosts(const std::string &, int = -1);
void appendTo(const std::string &, const std::string &, const std::string &, const std::string &);

class SNSServiceImpl final : public SNSService::Service
{

  Status List(ServerContext *context, const Request *request, ListReply *list_reply) override
  {
    std::string u = request->username();
    log(INFO, "List request from " + u);

    db_mutex.lock();
    // Add all users to the list reply
    for (const auto &client : client_db)
      list_reply->add_all_users(client.second->username);
    db_mutex.unlock();
    log(INFO, "Added all users to list reply");

    Client *c = getClient(u);

    // Add followers to the list reply
    log(INFO, "Attempting to add followers to list reply");
    if (c != nullptr)
    {
      db_mutex.lock();
      for (const auto &follower : c->client_followers)
      {
        // Cannot use getClient here, since it will lock the db_mutex again
        auto it = client_db.find(follower); // Get the iterator
        if (it != client_db.end())
        {                                                  // Check if the follower exists in the map
          list_reply->add_followers(it->second->username); // Access the Client* and add the username
        }
      }
      db_mutex.unlock();
      log(INFO, "Added followers to list reply");
    }

    return Status::OK;
  }

  Status Follow(ServerContext *context, const Request *request, Reply *reply) override
  {
    if (isMaster)
    {
      grpc::ClientContext serverContext;
      ID id;
      ServerList slaveServers;
      id.set_id(clusterID);
      grpc::Status status = coordinator_stub_->GetSlaves(&serverContext, id, &slaveServers);

      for (int i = 0; i < slaveServers.serverid_size(); i++)
      {
        std::string slaveAddr = slaveServers.hostname(i) + ":" + slaveServers.port(i);
        std::unique_ptr<csce438::SNSService::Stub> slave_stub_;
        slave_stub_ = SNSService::NewStub(grpc::CreateChannel(slaveAddr, grpc::InsecureChannelCredentials()));

        grpc::ClientContext clientContext;
        slave_stub_->Follow(&clientContext, *request, reply);
      }
    }

    std::string u1 = request->username();
    std::string u2 = request->arguments(0);

    log(INFO, "Follow request from " + u1 + " to " + u2);

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
      return Status::OK;
    }

    db_mutex.lock();
    c1->client_following.insert(c2->username);
    c2->client_followers.insert(c1->username);
    // not going to edit the c2 followers file, since when the synchronizer comes into play, it will move the follow_list to followers
    db_mutex.unlock();

    reply->set_msg("follow successful");

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
    if (isMaster)
    {
      grpc::ClientContext serverContext;
      ID id;
      ServerList slaveServers;
      id.set_id(clusterID);
      grpc::Status status = coordinator_stub_->GetSlaves(&serverContext, id, &slaveServers);

      for (int i = 0; i < slaveServers.serverid_size(); i++)
      {
        std::string slaveAddr = slaveServers.hostname(i) + ":" + slaveServers.port(i);
        std::unique_ptr<csce438::SNSService::Stub> slave_stub_;
        slave_stub_ = SNSService::NewStub(grpc::CreateChannel(slaveAddr, grpc::InsecureChannelCredentials()));

        grpc::ClientContext clientContext;
        slave_stub_->Login(&clientContext, *request, reply);
      }
    }

    Client *c = getClient(request->username());
    log(INFO, "Login request from " + request->username());

    if (c != nullptr)
    {
      if (c->connected)
      {
        reply->set_msg("user already connected");
      }
      else
      {
        db_mutex.lock();
        // c->connected = true; // need to be able to reconnect
        db_mutex.unlock();
        reply->set_msg("login succeeded");
      }
      return Status::OK;
    }
    else
    {
      log(INFO, "Creating new client " + request->username());
      Client *new_client = new Client();
      new_client->username = request->username();
      // new_client->connected = true; // need to be able to reconnect

      // Actual data modification
      db_mutex.lock();
      client_db[new_client->username] = new_client;
      db_mutex.unlock();
      reply->set_msg("login succeeded: new user created");
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

      curr = getClient(u);
      log(INFO, "Timeline request from client " + u);

      std::vector<Message> last_posts = getLastNPosts(u, 20);
      log(INFO, "Got last N posts for client " + u);

      for (const auto &post : last_posts)
      {
        stream->Write(post);
      }

      Message done;
      done.set_msg("DONE");
      stream->Write(done);
    }

    // thread to monitor u_following.txt file
    // I think it is ok for this thread to be here since this function can only be called once
    std::thread following([=]()
                          {
      log(INFO, "Starting following thread for client " + u);
      while (alive.load()) {
        // -1 indicates get all posts after followingFileLines
        log(INFO, "Getting last N posts for client " + u);
        std::vector<Message> newPosts = getLastNPosts(u, -1);
        for (const auto &post : newPosts)
          stream->Write(post);

        std::this_thread::sleep_for(std::chrono::seconds(5));
      } });

    // update client timeline file
    while (stream->Read(&m))
    {
      std::cout << "Server in cluster " << clusterID << " received message from client " << u << std::endl;
      log(INFO, "Received message from client " + u);
      std::time_t time = m.timestamp().seconds();
      std::tm *ltime = std::localtime(&time);
      std::stringstream ss;
      ss << std::put_time(ltime, "%Y-%m-%d %H:%M:%S");
      std::string time_str = ss.str();

      // have to do this withouth synchrnoizer
      if (curr != nullptr)
      {
        db_mutex.lock();
        log(INFO, "Writing message to client's followers");
        for (auto &f : curr->client_followers)
        {
          if (getClusterID(f) != clusterID)
          {
            log(INFO, "Client " + u + " is not in the same cluster as follower " + f);
            continue;
          }
          std::cout << "Server in cluster " << clusterID << " writing message to client " << u << "'s followers " << f << " following file" << std::endl;
          log(INFO, "Writing message to client " + u + "'s followers " + f + " following file");
          std::string followingFile = "./cluster_" + std::to_string(clusterID) + "/" + clusterSubdirectory + "/" + f + "_following.txt";
          std::string semName = "/" + std::to_string(clusterID) + "_" + clusterSubdirectory + "_" + f + "_following.txt";
          sem_t *fileSem = sem_open(semName.c_str(), O_CREAT, 0666, 1);

          sem_wait(fileSem);
          std::cout << "Server in cluster " << clusterID << " appending to following file for client " << u << "'s followers " << f << std::endl;
          appendTo(followingFile, time_str, u, m.msg());
          sem_post(fileSem);
          sem_close(fileSem);
          log(INFO, "Wrote message to client " + u + "'s followers " + f + " following file");
        }
        db_mutex.unlock();
      }
      else
      {
        log(ERROR, "Client not found: " + u);
      }

      std::cout << "Server in cluster " << clusterID << " writing received message from client " << u << " to timeline file" << std::endl;
      log(INFO, "Writing received message from client " + u + " to timeline file");
      appendTo("./cluster_" + std::to_string(clusterID) + "/" + clusterSubdirectory + "/" + u + "_timeline.txt", time_str, u, m.msg());
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
  {
    Client *client = it->second;
    db_mutex.unlock();
    return client;
  }
  db_mutex.unlock();
  log(ERROR, "Client not found: " + username);

  return nullptr;
}

// from username
int getClusterID(const std::string &username)
{
  int id = std::stoi(username);
  return ((id - 1) % 3) + 1;
}

std::vector<Message> getLastNPosts(const std::string &u, int n)
{
  std::vector<Message> posts;
  log(INFO, "Getting last N posts for user " + u);
  // Get last n posts from the user's following file
  std::string followingFile = "cluster_" + std::to_string(clusterID) + "/" + clusterSubdirectory + "/" + u + "_following.txt";
  std::string semName = "/" + std::to_string(clusterID) + "_" + clusterSubdirectory + "_" + u + "_following.txt";
  sem_t *fileSem = sem_open(semName.c_str(), O_CREAT, 0666, 1);

  sem_wait(fileSem);
  std::ifstream infile(followingFile);

  log(INFO, "Attempting to open following file for user " + u);
  if (!infile.is_open())
  {
    log(INFO, "No posts found for user " + u);
    sem_post(fileSem);
    sem_close(fileSem);
    return posts;
  }
  std::vector<std::string> lines;
  std::string line;

  while (std::getline(infile, line))
    lines.push_back(line);

  infile.close();
  sem_post(fileSem);
  sem_close(fileSem);
  log(INFO, "Got client " + u + " following's posts");

  int start = 0;

  if (n != -1)
  {
    // Initial fetch: Calculate start based purely on N, ignore stored value.
    start = std::max(0, static_cast<int>(lines.size()) - (n * 4));
    log(INFO, "Initial fetch (n=" + std::to_string(n) + "). Calculated start index: " + std::to_string(start));
  }
  else
  {
    // Background thread fetch: Use the stored value.
    ffl_mutex.lock();
    if (followingFileLines.find(u) == followingFileLines.end())
      followingFileLines[u] = 0; // Initialize if missing
    start = followingFileLines[u];
    ffl_mutex.unlock();
    log(INFO, "Background fetch (n=-1). Starting from stored index: " + std::to_string(start));
  }

  for (int i = start; i + 2 < lines.size(); i += 4)
  {
    if (lines[i].substr(0, 2) != "T " ||
        lines[i + 1].substr(0, 2) != "U " ||
        lines[i + 2].substr(0, 2) != "W ")
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

  // Create a stub to communicate with the coordinator
  std::string coordinator_address = coordinatorIP + ":" + coordinatorPort;
  coordinator_stub_ = csce438::CoordService::NewStub(grpc::CreateChannel(coordinator_address, grpc::InsecureChannelCredentials()));

  // Make directory for cluster
  std::string masterDir = "cluster_" + std::to_string(clusterID) + "/1";
  std::string slaveDir = "cluster_" + std::to_string(clusterID) + "/2";
  if (!std::filesystem::exists(masterDir))
  {
    std::filesystem::create_directories(masterDir);
  }
  if (!std::filesystem::exists(slaveDir))
  {
    std::filesystem::create_directories(slaveDir);
  }

  std::thread heartbeat([=]()
                        {
    bool registered = false;

    csce438::ServerInfo request;
    request.set_serverid(serverId);
    request.set_hostname("0.0.0.0");
    request.set_port(port_no);
    request.set_type("server");
    request.set_clusterid(clusterID);

    while (alive.load()) {
      grpc::ClientContext context;
      if (!registered){
        context.AddMetadata("clusterid", std::to_string(clusterID));
        registered = true;
      }

      csce438::Confirmation reply;
      grpc::Status status = coordinator_stub_->Heartbeat(&context, request, &reply);

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
      } else {
        log(ERROR, "Heartbeat failed");
      }

      std::this_thread::sleep_for(std::chrono::seconds(5));
    } });

  // update client db every 5 seconds
  std::thread db([&]()
                 {
    while (alive.load()) {
      std::this_thread::sleep_for(std::chrono::seconds(5));
      
      // add new clients to the client db
      db_mutex.lock();

      std::string usersFile = "cluster_" + std::to_string(clusterID) + "/" + clusterSubdirectory + "/all_users.txt";
      std::string semName = "/" + std::to_string(clusterID) + "_" + clusterSubdirectory + "_all_users.txt";
      sem_t *fileSem = sem_open(semName.c_str(), O_CREAT, 0666, 1);
      
      sem_wait(fileSem);
      
      // Read all existing users from file into sets
      std::unordered_set<std::string> users;
      std::ifstream readUserStream(usersFile);
      if (readUserStream.is_open()) {
        std::string user;
        while (readUserStream >> user) {
          users.insert(user);
          // Add users from file to client_db if they don't exist
          if (client_db.find(user) == client_db.end()) {
            Client *new_client = new Client();
            new_client->username = user;
            client_db[new_client->username] = new_client;
            log(INFO, "Added user " + user + " from file to client_db");
          }
        }
        readUserStream.close();
      } else {
        log(ERROR, "Error opening user file: " + usersFile);
      }

      // Check for users in client_db that are not in the file
      std::unordered_set<std::string> users_to_add;
      for (const auto &client : client_db) {
        if (users.find(client.first) == users.end()) {
          users_to_add.insert(client.first);
        }
      }

      // Rewrite file if necessary to add new users
      if (!users_to_add.empty()) {
        // Open file in truncate mode to rewrite completely
        std::ofstream writeUserStream(usersFile);
        if (writeUserStream.is_open()) {
          // Write all existing users first
          for (const auto &user : users) {
            writeUserStream << user << std::endl;
          }
          // Then add new users
          for (const auto &user : users_to_add) {
            writeUserStream << user << std::endl;
            log(INFO, "Client " + user + " added to all_users file");
          }
          writeUserStream.close();
        } else {
          log(ERROR, "Error opening user file for writing: " + usersFile);
        }
      }
      
      sem_post(fileSem);
      sem_close(fileSem);

      //follow list file
      for(const auto &client : client_db){
        if(getClusterID(client.first) == clusterID){
          std::string file = "cluster_" + std::to_string(clusterID) + "/" + clusterSubdirectory + "/" + client.first + "_follow_list.txt";
          std::string semName = "/" + std::to_string(clusterID) + "_" + clusterSubdirectory + "_" + client.first + "_follow_list.txt";
          fileSem = sem_open(semName.c_str(), O_CREAT, 0666, 1);
          sem_wait(fileSem);
          
          // Read existing follow list
          std::unordered_set<std::string> follows;
          std::ifstream readFollowingStream(file, std::ios::in);
          if (readFollowingStream.is_open()) {
            log(INFO, "Follow List file opened successfully for " + client.first);
            std::string following;
            while (readFollowingStream >> following) {
              follows.insert(following);
              if(client.second->client_following.find(following) == client.second->client_following.end()){
                client.second->client_following.insert(following);
              }
            }
            readFollowingStream.close();
          } else {
            log(ERROR, "Error opening following file: " + file);
          }

          // Check for new follows in memory that aren't in file
          std::unordered_set<std::string> follows_to_add;
          for (const auto &follow : client.second->client_following) {
            if (follows.find(follow) == follows.end()) {
              follows_to_add.insert(follow);
            }
          }

          // Only rewrite file if there are new follows to add
          if (!follows_to_add.empty()) {
            std::ofstream writeFollowingStream(file);
            if (writeFollowingStream.is_open()) {
              // Write all existing follows
              for (const auto &follow : follows) {
                writeFollowingStream << follow << std::endl;
              }
              // Add new follows
              for (const auto &follow : follows_to_add) {
                writeFollowingStream << follow << std::endl;
                log(INFO, "Added " + follow + " to " + client.first + "'s follow list file");
              }
              writeFollowingStream.close();
            } else {
              log(ERROR, "Error opening follow list file for writing: " + file);
            }
          }
          
          sem_post(fileSem);
          sem_close(fileSem);
        }
      }

      // follower file
      for(const auto &client : client_db) {
        Client *c1 = client.second;
        std::string file = "cluster_" + std::to_string(clusterID) + "/" + clusterSubdirectory + "/" + client.first + "_followers.txt";
        semName = "/" + std::to_string(clusterID) + "_" + clusterSubdirectory + "_" + client.first + "_followers.txt";
        fileSem = sem_open(semName.c_str(), O_CREAT, 0666, 1);

        sem_wait(fileSem);
        
        // Read existing followers
        std::unordered_set<std::string> followers;
        std::ifstream followerStream(file, std::ios::in);
        if (followerStream.is_open()) {
          log(INFO, "Follower file opened successfully for " + client.first);
          std::string follower;
          while (followerStream >> follower) {
            followers.insert(follower);
            if (c1->client_followers.find(follower) == c1->client_followers.end()) {
              c1->client_followers.insert(follower);
            }
          }
          followerStream.close();
        } else {
          log(ERROR, "Error opening follower file: " + file);
          sem_post(fileSem);
          sem_close(fileSem);
          continue;
        }

        // Check for new followers in memory that aren't in file
        std::unordered_set<std::string> followers_to_add;
        for (const auto &follower : c1->client_followers) {
          if (followers.find(follower) == followers.end()) {
            followers_to_add.insert(follower);
          }
        }

        // Only rewrite file if there are new followers to add
        if (!followers_to_add.empty()) {
          std::ofstream writeFollowerStream(file);
          if (writeFollowerStream.is_open()) {
            // Write all existing followers
            for (const auto &follower : followers) {
              writeFollowerStream << follower << std::endl;
            }
            // Add new followers
            for (const auto &follower : followers_to_add) {
              writeFollowerStream << follower << std::endl;
              log(INFO, "Added " + follower + " to " + client.first + "'s followers file");
            }
            writeFollowerStream.close();
          } else {
            log(ERROR, "Error opening followers file for writing: " + file);
          }
        }
        
        sem_post(fileSem);
        sem_close(fileSem);
      }
      
      db_mutex.unlock();
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
