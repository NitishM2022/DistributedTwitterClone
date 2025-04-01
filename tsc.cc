#include <iostream>
#include <memory>
#include <thread>
#include <vector>
#include <string>
#include <unistd.h>
#include <csignal>
#include "client.h"

#include <google/protobuf/util/time_util.h>
#include <grpc++/grpc++.h>
#include <glog/logging.h>
#define log(severity, msg) \
  LOG(severity) << msg;    \
  google::FlushLogFiles(google::severity);

#include "sns.grpc.pb.h"
#include "coordinator.grpc.pb.h"

using grpc::Channel;
using grpc::ClientContext;
using grpc::ClientReader;
using grpc::ClientReaderWriter;
using grpc::ClientWriter;
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

void sig_ignore(int sig)
{
  std::cout << "Signal caught " + sig;
}

Message MakeMessage(const std::string &username, const std::string &msg)
{
  Message m;
  m.set_username(username);
  m.set_msg(msg);
  google::protobuf::Timestamp *timestamp = new google::protobuf::Timestamp();
  timestamp->set_seconds(time(NULL));
  timestamp->set_nanos(0);
  m.set_allocated_timestamp(timestamp);
  return m;
}

class Client : public IClient
{
public:
  Client(const std::string &hname,
         const std::string &uname,
         const std::string &p)
      : hostname(hname), username(uname), port(p) {}

protected:
  virtual int connectTo();
  virtual IReply processCommand(std::string &input);
  virtual void processTimeline();

private:
  std::string hostname;
  std::string username;
  std::string port;

  // You can have an instance of the client stub
  // as a member variable.
  std::unique_ptr<SNSService::Stub> stub_;

  IReply Login();
  IReply List();
  IReply Follow(const std::string &username);
  IReply UnFollow(const std::string &username);
  void Timeline(const std::string &username);
};

///////////////////////////////////////////////////////////
//
//////////////////////////////////////////////////////////
int Client::connectTo()
{
  // ------------------------------------------------------------
  // In this function, you are supposed to create a stub so that
  // you call service methods in the processCommand/porcessTimeline
  // functions. That is, the stub should be accessible when you want
  // to call any service methods in those functions.
  // Please refer to gRpc tutorial how to create a stub.
  // ------------------------------------------------------------

  std::string login_info = hostname + ":" + port;
  stub_ = SNSService::NewStub(grpc::CreateChannel(login_info, grpc::InsecureChannelCredentials()));
  IReply reply = Login();
  if (reply.comm_status != SUCCESS)
  {
    return -1;
  }
  return 1;
}

IReply Client::processCommand(std::string &input)
{
  // ------------------------------------------------------------
  // GUIDE 1:
  // In this function, you are supposed to parse the given input
  // command and create your own message so that you call an
  // appropriate service method. The input command will be one
  // of the followings:
  //
  // FOLLOW <username>
  // UNFOLLOW <username>
  // LIST
  // TIMELINE
  // ------------------------------------------------------------

  // ------------------------------------------------------------
  // GUIDE 2:
  // Then, you should create a variable of IReply structure
  // provided by the client.h and initialize it according to
  // the result. Finally you can finish this function by returning
  // the IReply.
  // ------------------------------------------------------------

  // ------------------------------------------------------------
  // HINT: How to set the IReply?
  // Suppose you have "FOLLOW" service method for FOLLOW command,
  // IReply can be set as follow:
  //
  //     // some codes for creating/initializing parameters for
  //     // service method
  //     IReply ire;
  //     grpc::Status status = stub_->FOLLOW(&context, /* some parameters */);
  //     ire.grpc_status = status;
  //     if (status.ok()) {
  //         ire.comm_status = SUCCESS;
  //     } else {
  //         ire.comm_status = FAILURE_NOT_EXISTS;
  //     }
  //
  //      return ire;
  //
  // IMPORTANT:
  // For the command "LIST", you should set both "all_users" and
  // "following_users" member variable of IReply.
  // ------------------------------------------------------------

  IReply ire;

  if (input.size() >= 6 && input.substr(0, 6) == "FOLLOW")
  {
    std::string arg = input.substr(7);
    ire = Follow(arg);
  }
  else if (input.size() >= 8 && input.substr(0, 8) == "UNFOLLOW")
  {
    std::string arg = input.substr(9);
    ire = UnFollow(arg);
  }
  else if (input == "LIST")
  {
    ire = List();
  }
  else if (input == "TIMELINE")
  {
    // timeline has no way to return a reply so we use list to test connection
    ire = List();
  }
  else
  {
    ire.comm_status = FAILURE_INVALID;
  }

  return ire;
}

void Client::processTimeline()
{
  Timeline(username);
}

// List Command
IReply Client::List()
{
  IReply ire;

  ClientContext context;
  Request request;
  request.set_username(username);
  ListReply list_reply;

  Status status = stub_->List(&context, request, &list_reply);
  ire.grpc_status = status;
  if (status.ok())
  {
    ire.comm_status = SUCCESS;
    for (const auto &user : list_reply.all_users())
    {
      ire.all_users.push_back(user);
    }
    for (const auto &follower : list_reply.followers())
    {
      ire.followers.push_back(follower);
    }
  }
  else
  {
    ire.comm_status = FAILURE_UNKNOWN;
  }

  return ire;
}

// Follow Command
IReply Client::Follow(const std::string &username2)
{
  IReply ire;

  ClientContext context;
  Request request;
  request.set_username(username);
  request.add_arguments(username2);
  Reply reply;

  Status status = stub_->Follow(&context, request, &reply);
  ire.grpc_status = status;

  if (status.ok())
  {
    if (reply.msg() == "follow successful")
    {
      ire.comm_status = SUCCESS;
    }
    else if (reply.msg() == "cannot follow yourself" || reply.msg() == "already following")
    {
      ire.comm_status = FAILURE_ALREADY_EXISTS;
    }
    else if (reply.msg() == "user to follow does not exist")
    {
      ire.comm_status = FAILURE_INVALID_USERNAME;
    }
    else
    {
      ire.comm_status = FAILURE_UNKNOWN;
    }
  }
  else
  {
    ire.comm_status = FAILURE_UNKNOWN;
  }

  return ire;
}

// UNFollow Command
IReply Client::UnFollow(const std::string &username2)
{
  IReply ire;

  ClientContext context;
  Request request;
  request.set_username(username);
  request.add_arguments(username2);
  Reply reply;

  Status status = stub_->UnFollow(&context, request, &reply);
  ire.grpc_status = status;

  if (status.ok())
  {
    if (reply.msg() == "unfollow successful")
    {
      ire.comm_status = SUCCESS;
    }
    else if (reply.msg() == "not following")
    {
      ire.comm_status = FAILURE_INVALID_USERNAME;
    }
    else
    {
      ire.comm_status = FAILURE_UNKNOWN;
    }
  }
  else
  {
    ire.comm_status = FAILURE_UNKNOWN;
  }

  return ire;
}

// Login Command
IReply Client::Login()
{
  IReply ire;

  Request request;
  request.set_username(username);
  Reply reply;
  ClientContext context;

  Status status = stub_->Login(&context, request, &reply);
  ire.grpc_status = status;

  if (status.ok())
  {
    if (reply.msg() == "login succeeded" || reply.msg() == "login succeeded: new user created")
    {
      ire.comm_status = SUCCESS;
    }
    else if (reply.msg() == "user already connected")
    {
      ire.comm_status = FAILURE_ALREADY_EXISTS;
    }
    else
    {
      ire.comm_status = FAILURE_UNKNOWN;
    }
  }
  else
  {
    ire.comm_status = FAILURE_UNKNOWN;
  }

  return ire;
}

// Timeline Command
void Client::Timeline(const std::string &username)
{

  // ------------------------------------------------------------
  // In this function, you are supposed to get into timeline mode.
  // You may need to call a service method to communicate with
  // the server. Use getPostMessage/displayPostMessage functions
  // in client.cc file for both getting and displaying messages
  // in timeline mode.
  // ------------------------------------------------------------

  // ------------------------------------------------------------
  // IMPORTANT NOTICE:
  //
  // Once a user enter to timeline mode , there is no way
  // to command mode. You don't have to worry about this situation,
  // and you can terminate the client program by pressing
  // CTRL-C (SIGINT)
  // ------------------------------------------------------------

  ClientContext context;
  std::shared_ptr<ClientReaderWriter<Message, Message>> stream(stub_->Timeline(&context));
  
  Message init = MakeMessage(username, "INIT");
  stream->Write(init);

  Message initial_posts;
  while (stream->Read(&initial_posts) && initial_posts.msg() != "DONE")
  {
    google::protobuf::Timestamp timestamp = initial_posts.timestamp();
    std::time_t time = timestamp.seconds();
    displayPostMessage(initial_posts.username(), initial_posts.msg(), time);
  }

  std::thread writer_thread([stream, this, username]()
                            {
    while (true) {
      std::string message = getPostMessage();
      Message msg = MakeMessage(username, message);
      stream->Write(msg);
    } });

  std::thread reader_thread([stream, this]()
                            {
    Message msg;
    while (stream->Read(&msg)) {
      google::protobuf::Timestamp timestamp = msg.timestamp();
        std::time_t time = timestamp.seconds();
        displayPostMessage(msg.username(), msg.msg(), time);
    } });

  writer_thread.join();
  reader_thread.join();
}

//////////////////////////////////////////////
// Main Function
/////////////////////////////////////////////
int main(int argc, char **argv)
{

  std::string coordinatorIP = "localhost";
  std::string coordinatorPort = "3010";
  std::string username = "default";


  int opt = 0;
  while ((opt = getopt(argc, argv, "h:k:u:")) != -1)
  {
    switch (opt)
    {
    case 'h':
      coordinatorIP = optarg;
      break;
    case 'k':
      coordinatorPort = optarg;
      break;
    case 'u':
      username = optarg;
      break;
    default:
      std::cerr << "Invalid Command Line Argument\n";
      log(ERROR, "Invalid Command Line Argument");
      return 1;
    }
  }

  std::string coordinator_address = coordinatorIP + ":" + coordinatorPort;
  std::unique_ptr<csce438::CoordService::Stub> stub = csce438::CoordService::NewStub(grpc::CreateChannel(coordinator_address, grpc::InsecureChannelCredentials()));

  csce438::ID id;
  id.set_id(atoi(username.c_str()));
  csce438::ServerInfo serverInfo;

  log(INFO, "Getting server info from coordinator");
  grpc::ClientContext context;
  grpc::Status status = stub->GetServer(&context, id, &serverInfo);

  std::string hostname = serverInfo.hostname();
  std::string port = serverInfo.port();

  std::cout << "Logging Initialized. Client starting...";

  Client myc(hostname, username, port);
  myc.run();

  return 0;
}
