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
#include <iomanip>
#include <memory>
#include <string>
#include <sstream>
#include <stdlib.h>
#include <unistd.h>
#include <thread>
#include <atomic>

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
  std::vector<Client *> client_followers;
  std::vector<Client *> client_following;
  ServerReaderWriter<Message, Message> *stream = 0;
  bool operator==(const Client &c1) const
  {
    return (username == c1.username);
  }
};

// Vector that stores every client that has been created
std::vector<Client *> client_db;
std::mutex client_mutex;
class SNSServiceImpl final : public SNSService::Service
{

  Status List(ServerContext *context, const Request *request, ListReply *list_reply) override
  {
    Client *c = nullptr;
    for (auto &client : client_db)
    {
      if (client->username == request->username())
      {
        c = client;
        break;
      }
    }
    if (c != nullptr)
    {
      for (auto &client : client_db)
      {
        list_reply->add_all_users(client->username);
      }
      for (auto &follower : c->client_followers)
      {
        list_reply->add_followers(follower->username);
      }
    }

    return Status::OK;
  }

  Status Follow(ServerContext *context, const Request *request, Reply *reply) override
  {
    std::string u1 = request->username();
    std::string u2 = request->arguments(0);

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
    for (auto &following : c1->client_following)
    {
      if (following->username == u2)
      {
        reply->set_msg("already following");
        return Status::OK;
      }
    }

    c1->client_following.push_back(c2);
    c2->client_followers.push_back(c1);
    reply->set_msg("follow successful");

    return Status::OK;
  }

  Status UnFollow(ServerContext *context, const Request *request, Reply *reply) override
  {
    std::string u1 = request->username();
    std::string u2 = request->arguments(0);
    Client *c1 = getClient(u1);
    Client *c2 = getClient(u2);

    auto it = std::find(c1->client_following.begin(), c1->client_following.end(), c2);
    if (it == c1->client_following.end())
    {
      reply->set_msg("not following");
      return Status::OK;
    }

    c1->client_following.erase(it);
    c2->client_followers.erase(std::remove(c2->client_followers.begin(), c2->client_followers.end(), c1), c2->client_followers.end());
    reply->set_msg("unfollow successful");

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
      client_db.push_back(new_client);
      reply->set_msg("login succeeded: new user created");
    }

    return Status::OK;
  }

  Status Timeline(ServerContext *context,
                  ServerReaderWriter<Message, Message> *stream) override
  {
    Message m;
    bool init = true;
    Client *curr = nullptr;

    while (stream->Read(&m))
    {
      if (init)
      {
        std::string u = m.username();
        init = false;

        curr = getClient(u);
        if (curr != nullptr)
        {
          curr->stream = stream;
        }

        std::vector<Message> last_posts = getLastNPosts(u + "_following.txt");

        for (const auto &post : last_posts)
        {
          stream->Write(post);
        }

        Message done;
        done.set_msg("DONE");
        stream->Write(done);
      }
      else
      {
        std::string u = m.username();
        Client *c = getClient(u);
        if (c == nullptr)
        {
          continue;
        }

        std::time_t time = m.timestamp().seconds();
        std::tm *ltime = std::localtime(&time);
        std::stringstream ss;
        ss << std::put_time(ltime, "%Y-%m-%d %H:%M:%S");
        std::string time_str = ss.str();

        appendTo(u + ".txt", time_str, u, m.msg());

        for (auto &f : c->client_followers)
        {
          if (f->stream != nullptr)
          {
            Message follower_msg = m;
            follower_msg.set_username(u);
            follower_msg.mutable_timestamp()->set_seconds(time);
            f->stream->Write(follower_msg);
          }
          appendTo(f->username + "_following.txt", time_str, u, m.msg());
        }
      }
    }

    return Status::OK;
  }

  Client *getClient(const std::string &username)
  {
    for (auto &client : client_db)
    {
      if (client->username == username)
      {
        return client;
      }
    }
    return nullptr;
  }

  std::vector<Message> getLastNPosts(const std::string &following_file, int n = 20)
  {
    std::vector<Message> posts;
    std::ifstream infile(following_file);
    if (!infile.is_open())
      return posts;

    std::vector<std::string> lines;
    std::string line;

    while (std::getline(infile, line))
    {
      lines.push_back(line);
    }
    infile.close();

    int total_lines = lines.size();
    int count = 0;

    for (int i = total_lines - 4; i >= 0; i -= 4)
    {
      if (count >= n)
        break;

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
        std::cerr << "Error parsing time string: " << timestamp_str << std::endl;
        continue;
      }

      Message msg;
      google::protobuf::Timestamp *timestamp = msg.mutable_timestamp();
      timestamp->set_seconds(post_time);
      msg.set_username(username);
      msg.set_msg(message);

      posts.push_back(std::move(msg));
      count++;
    }

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
      std::cerr << "Error opening file: " << filename << std::endl;
    }
  }
};

void RunServer(int clusterId, int serverId, std::string port_no, std::string coordinatorIP, std::string coordinatorPort)
{
  std::string server_address = "0.0.0.0:" + port_no;
  SNSServiceImpl service;

  ServerBuilder builder;
  builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);
  std::unique_ptr<Server> server(builder.BuildAndStart());
  std::cout << "Server listening on " << server_address << std::endl;
  log(INFO, "Server listening on " + server_address);

  std::atomic<bool> alive = true;

  std::thread heartbeat([&]()
                        {
    bool registered = false;
    std::string coordinator_address = coordinatorIP + ":" + coordinatorPort;
    while (alive.load()) {
      std::unique_ptr<csce438::CoordService::Stub> stub = csce438::CoordService::NewStub(grpc::CreateChannel(coordinator_address, grpc::InsecureChannelCredentials()));

      csce438::ServerInfo request;
      request.set_serverid(serverId);
      request.set_hostname("0.0.0.0");
      request.set_port(port_no);
      request.set_type("S");

      grpc::ClientContext context;
      if (!registered){
        context.AddMetadata("clusterid", std::to_string(clusterId));
      }

      csce438::Confirmation reply;
      grpc::Status status = stub->Heartbeat(&context, request, &reply);

      if (status.ok()) {
        log(INFO, "Heartbeat sent successfully.");
        if (!registered){
          registered = true;
        }
      } else {
        log(ERROR, "Heartbeat failed: " + status.error_message());
      }

      std::this_thread::sleep_for(std::chrono::seconds(5));
    } });

  server->Wait();

  alive = false;
  heartbeat.join();
}

int main(int argc, char **argv)
{

  int clusterId = 1;
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
      clusterId = atoi(optarg);
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
  RunServer(clusterId, serverId, port, coordinatorIP, coordinatorPort);

  return 0;
}
