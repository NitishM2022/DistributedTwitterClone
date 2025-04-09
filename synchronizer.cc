// NOTE: This starter code contains a primitive implementation using the default RabbitMQ protocol.
// You are recommended to look into how to make the communication more efficient,
// for example, modifying the type of exchange that publishes to one or more queues, or
// throttling how often a process consumes messages from a queue so other consumers are not starved for messages
// All the functions in this implementation are just suggestions and you can make reasonable changes as long as
// you continue to use the communication methods that the assignment requires between different processes

#include <bits/fs_fwd.h>
#include <ctime>
#include <google/protobuf/timestamp.pb.h>
#include <google/protobuf/duration.pb.h>
#include <chrono>
#include <semaphore.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unordered_map>
#include <vector>
#include <unordered_set>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <mutex>
#include <stdlib.h>
#include <stdio.h>
#include <cstdlib>
#include <unistd.h>
#include <algorithm>
#include <google/protobuf/util/time_util.h>
#include <grpc++/grpc++.h>
#include <glog/logging.h>
#include "sns.grpc.pb.h"
#include "sns.pb.h"
#include "coordinator.grpc.pb.h"
#include "coordinator.pb.h"

#include <amqp.h>
#include <amqp_tcp_socket.h>
#include <jsoncpp/json/json.h>

#define log(severity, msg) \
    LOG(severity) << msg;  \
    google::FlushLogFiles(google::severity);

namespace fs = std::filesystem;

using csce438::AllUsers;
using csce438::Confirmation;
using csce438::CoordService;
using csce438::ID;
using csce438::ServerInfo;
using csce438::ServerList;
using csce438::SynchronizerListReply;
using csce438::SynchService;
using google::protobuf::Duration;
using google::protobuf::Timestamp;
using grpc::ClientContext;
using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerReader;
using grpc::ServerReaderWriter;
using grpc::ServerWriter;
using grpc::Status;
// tl = timeline, fl = follow list
using csce438::TLFL;

int synchID = 1;
int clusterID = 1;
bool isMaster = false;
int total_number_of_registered_synchronizers = 6; // update this by asking coordinator
std::string coordAddr;
std::string clusterSubdirectory = "2"; // default to slave
std::vector<std::string> otherHosts;
std::unordered_map<std::string, int> timelineLengths;

std::vector<std::string> get_lines_from_file(std::string, std::string);
std::vector<std::string> get_all_users_func(int);
std::vector<std::string> get_tl_or_fl(int, int, bool);
std::vector<std::string> getFollowersOfUser(int);
bool file_contains_user(std::string filename, std::string user);

void Heartbeat(std::string coordinatorIp, std::string coordinatorPort, ServerInfo serverInfo, int syncID);

std::unique_ptr<csce438::CoordService::Stub> coordinator_stub_;

class SynchronizerRabbitMQ
{
private:
    amqp_connection_state_t conn;
    amqp_channel_t channel;
    std::string hostname;
    int port;
    int synchID;

    void setupRabbitMQ()
    {
        conn = amqp_new_connection();
        amqp_socket_t *socket = amqp_tcp_socket_new(conn);
        amqp_socket_open(socket, hostname.c_str(), port);
        amqp_login(conn, "/", 0, 131072, 0, AMQP_SASL_METHOD_PLAIN, "guest", "guest");
        amqp_channel_open(conn, channel);
    }

    void declareQueue(const std::string &queueName)
    {
        amqp_queue_declare(conn, channel, amqp_cstring_bytes(queueName.c_str()),
                           0, 0, 0, 0, amqp_empty_table);
    }

    void publishMessage(const std::string &queueName, const std::string &message)
    {
        amqp_basic_publish(conn, channel, amqp_empty_bytes, amqp_cstring_bytes(queueName.c_str()),
                           0, 0, NULL, amqp_cstring_bytes(message.c_str()));
    }

public:
    std::pair<std::string, std::string> consumeMessage(int timeout_ms = 5000)
    {
        amqp_envelope_t envelope;
        amqp_maybe_release_buffers(conn);

        struct timeval timeout;
        timeout.tv_sec = timeout_ms / 1000;
        timeout.tv_usec = (timeout_ms % 1000) * 1000;

        amqp_rpc_reply_t res = amqp_consume_message(conn, &envelope, &timeout, 0);

        if (res.reply_type != AMQP_RESPONSE_NORMAL)
        {
            return {"", ""}; // Return empty message and routing key on failure
        }

        std::string message(static_cast<char *>(envelope.message.body.bytes), envelope.message.body.len);
        std::string routing_key((char *)envelope.routing_key.bytes, envelope.routing_key.len);
        amqp_destroy_envelope(&envelope);
        return {message, routing_key};
    }
    
    // SynchronizerRabbitMQ(const std::string &host, int p, int id) : hostname(host), port(p), channel(1), synchID(id)
    SynchronizerRabbitMQ(const std::string &host, int p, int id) : hostname("rabbitmq"), port(p), channel(1), synchID(id)
    {
        setupRabbitMQ();
        declareQueue("synch" + std::to_string(synchID) + "_users_queue");
        declareQueue("synch" + std::to_string(synchID) + "_clients_relations_queue");
        declareQueue("synch" + std::to_string(synchID) + "_timeline_queue");
        // TODO: add or modify what kind of queues exist in your clusters based on your needs

        std::string queueName = "synch" + std::to_string(synchID) + "_users_queue";
        amqp_basic_consume(conn, channel, amqp_cstring_bytes(queueName.c_str()),
                           amqp_empty_bytes, 0, 0, 0, amqp_empty_table);

        queueName = "synch" + std::to_string(synchID) + "_clients_relations_queue";
        amqp_basic_consume(conn, channel, amqp_cstring_bytes(queueName.c_str()),
                           amqp_empty_bytes, 0, 0, 0, amqp_empty_table);

        queueName = "synch" + std::to_string(synchID) + "_timeline_queue";
        amqp_basic_consume(conn, channel, amqp_cstring_bytes(queueName.c_str()),
                           amqp_empty_bytes, 0, 0, 0, amqp_empty_table);
        
    }

    void publishUserList()
    {
        std::vector<std::string> users = get_all_users_func(synchID);
        std::sort(users.begin(), users.end());
        Json::Value userList;
        for (const auto &user : users)
        {
            userList["users"].append(user);
        }
        Json::FastWriter writer;
        std::string message = writer.write(userList);

        grpc::ClientContext context;
        csce438::ServerList followerServers;
        csce438::ID id;
        id.set_id(synchID);

        grpc::Status status = coordinator_stub_->GetAllFollowerServers(&context, id, &followerServers);

        if (status.ok())
        {
            total_number_of_registered_synchronizers = followerServers.serverid_size();

            for (int i = 1; i <= total_number_of_registered_synchronizers; i++)
            {
                std::string queueName = "synch" + std::to_string(i) + "_users_queue";
                publishMessage("synch" + std::to_string(synchID) + "_users_queue", message);
                log(INFO, "Published user list to " + queueName);
            }
        }
        else
        {
            log(ERROR, "Failed to get follower server list from coordinator: " + status.error_message());
        }
    }

    void consumeUserLists(const std::string& message)
    {
        std::vector<std::string> allUsers;

        // YOUR CODE HERE
        log(INFO, "Received user list");
        log(INFO, "Message: " + message);
        if (!message.empty())
        {
            Json::Value root;
            Json::Reader reader;
            if (reader.parse(message, root))
            {
                for (const auto &user : root["users"])
                {
                    log(INFO, "Received user: " + user.asString());
                    allUsers.push_back(user.asString());
                }
            }
        }

        updateAllUsersFile(allUsers);
    }

    void publishClientRelations()
    {
        Json::Value relations;
        std::vector<std::string> users = get_all_users_func(synchID);

        for (const auto &client : users)
        {
            log(INFO, "Publishing client relations for client " + client);
            int clientId = std::stoi(client);
            std::vector<std::string> followers = getFollowersOfUser(clientId);

            Json::Value followerList(Json::arrayValue);
            for (const auto &follower : followers)
            {
                followerList.append(follower);
            }

            if (!followerList.empty())
            {
                relations[client] = followerList;
            }
        }

        grpc::ClientContext context;
        csce438::ServerList followerServers;
        csce438::ID id;
        id.set_id(synchID);

        // TODO: hardcoding 6 here, but you need to get list of all synchronizers from coordinator as before
        grpc::Status status = coordinator_stub_->GetAllFollowerServers(&context, id, &followerServers);

        if (!status.ok())
        {
            log(ERROR, "Failed to get follower server list from coordinator: " + status.error_message());
            return;
        }

        total_number_of_registered_synchronizers = followerServers.serverid_size();

        for (int i = 1; i <= total_number_of_registered_synchronizers; i++)
        {
            std::string queueName = "synch" + std::to_string(i) + "_clients_relations_queue";
            Json::FastWriter writer;
            std::string message = writer.write(relations);
            publishMessage(queueName, message);
            log(INFO, "Published client relations to " + queueName);
        }
    }

    void consumeClientRelations(const std::string& message)
    {
        std::vector<std::string> allUsers = get_all_users_func(synchID);

        // YOUR CODE HERE
        log(INFO, "Received client relations ");
        log(INFO, "Message: " + message);

        if (!message.empty())
        {
            Json::Value root;
            Json::Reader reader;
            if (reader.parse(message, root))
            {
                for (const auto &client : allUsers)
                {
                    std::string followerFile = "./cluster_" + std::to_string(clusterID) + "/" + clusterSubdirectory + "/" + client + "_followers.txt";
                    std::string semName = "/" + std::to_string(clusterID) + "_" + clusterSubdirectory + "_" + client + "_followers.txt";
                    sem_t *fileSem = sem_open(semName.c_str(), O_CREAT, 0666, 1);

                    sem_wait(fileSem);
                    std::ofstream followerStream(followerFile, std::ios::app | std::ios::out | std::ios::in);
                    if (root.isMember(client))
                    {
                        for (const auto &follower : root[client])
                        {
                            if (!file_contains_user(followerFile, follower.asString()))
                            {
                                followerStream << follower.asString() << std::endl;
                            }
                        }
                    }
                    followerStream.close();
                    sem_post(fileSem);
                    sem_close(fileSem);
                }
            }
        }
    }

    // for every client in your cluster, update all their followers' timeline files
    // by publishing your user's timeline file (or just the new updates in them)
    //  periodically to the message queue of the synchronizer responsible for that client
    void publishTimelines()
    {
        std::vector<std::string> users = get_all_users_func(synchID);

        for (const auto &client : users)
        {
            log(INFO, "Publishing timeline for client " + client);
            int clientId = std::stoi(client);
            int client_cluster = ((clientId - 1) % 3) + 1;
            // only do this for clients in your own cluster
            if (client_cluster != clusterID)
            {
                continue;
            }

            std::vector<std::string> timeline = get_tl_or_fl(synchID, clientId, true);
            if (timeline.empty())
            {
                continue;
            }

            std::vector<std::string> followers = getFollowersOfUser(clientId);

            for (const auto &follower : followers)
            {
                // send the timeline updates of your current user to all its followers

                // YOUR CODE HERE
                Json::Value timeline_json;
                timeline_json[client] = Json::arrayValue;
                for (size_t i = 0; i < timeline.size(); i += 4)
                {
                    if (i + 2 >= timeline.size() || timeline[i].substr(0, 2) != "T ")
                        continue;

                    std::string timestamp = timeline[i].substr(2);
                    std::string username = timeline[i + 1].substr(2);
                    std::string message = timeline[i + 2].substr(2);

                    Json::Value post;
                    post["timestamp"] = timestamp;
                    post["username"] = username;
                    post["message"] = message;
                    timeline_json[client].append(post);
                }
                Json::FastWriter writer;
                std::string message = writer.write(timeline_json);

                grpc::ClientContext context;
                csce438::ServerList followerServers;
                csce438::ID id;
                id.set_id(synchID);

                // need to write to the follower's synchronizer queue
                grpc::Status status = coordinator_stub_->GetAllFollowerServers(&context, id, &followerServers);

                if (!status.ok())
                {
                    log(ERROR, "Failed to get follower server list from coordinator: " + status.error_message());
                    return;
                }

                log(INFO, "Attempting to publish to follower " + follower);
                int followerId = std::stoi(follower);
                int followerClusterID = ((followerId - 1) % 3) + 1;

                total_number_of_registered_synchronizers = followerServers.serverid_size();
                for (int i = 0; i < followerServers.serverid_size(); ++i)
                {
                    // Send to the follower's synchronizer,
                    // Ensure that you are not sending follower updates if they are in the same cluster
                    // Ensure you are not sending to yourself
                    log(INFO, "Checking cluster " + followerServers.clusterid(i));
                    if (std::stoi(followerServers.clusterid(i)) == followerClusterID && client_cluster != followerClusterID && followerServers.serverid(i) != synchID)
                    {
                        std::string queueName = "synch" + std::to_string(followerServers.serverid(i)) + "_timeline_queue";
                        publishMessage(queueName, message);
                        log(INFO, "Published timeline update to " + queueName);
                    }
                }
            }
        }
    }

    // For each client in your cluster, consume messages from your timeline queue and modify your client's timeline files based on what the users they follow posted to their timeline
    void consumeTimelines(const std::string& message)
    {
        log(INFO, "Received timeline update ");
        log(INFO, "Message: " + message);

        if (!message.empty())
        {
            // consume the message from the queue and update the timeline file of the appropriate client with
            // the new updates to the timeline of the user it follows

            // YOUR CODE HERE
            Json::Value root;
            Json::Reader reader;
            if (reader.parse(message, root))
            {
                for (const auto &clientId : root.getMemberNames())
                {
                    std::string timelineFile = "./cluster_" + std::to_string(clusterID) + "/" + clusterSubdirectory + "/" + clientId + "_timeline.txt";
                    std::ofstream timelineStream(timelineFile, std::ios::app | std::ios::out | std::ios::in);

                    if (timelineLengths.find(clientId) == timelineLengths.end())
                    {
                        timelineLengths[clientId] = 0;
                    }

                    log(INFO, "Updating timeline for client " + clientId);
                    std::vector<std::string> followers = getFollowersOfUser(std::stoi(clientId));
                    int followingLength = timelineLengths[clientId];

                    for (int i = timelineLengths[clientId]; i < root[clientId].size(); i++)
                    {
                        const auto &post = root[clientId][i];
                        timelineStream << "T " << post["timestamp"].asString() << "\n";
                        timelineStream << "U " << post["username"].asString() << "\n";
                        timelineStream << "W " << post["message"].asString() << "\n";
                        timelineStream << "\n";
                        timelineLengths[clientId]++;
                    }

                    // Update the following file as well
                    for (const auto &follower : followers)
                    {
                        std::string followingFile = "./cluster_" + std::to_string(clusterID) + "/" + clusterSubdirectory + "/" + follower + "_following.txt";
                        std::string semName = "/" + std::to_string(clusterID) + "_" + clusterSubdirectory + "_" + follower + "_following.txt";
                        sem_t *fileSem = sem_open(semName.c_str(), O_CREAT, 0666, 1);

                        sem_wait(fileSem);
                        std::ofstream followingStream(followingFile, std::ios::app | std::ios::out | std::ios::in);

                        for (int i = followingLength; i < root[clientId].size(); i++)
                        {
                            const auto &post = root[clientId][i];
                            followingStream << "T " << post["timestamp"].asString() << "\n";
                            followingStream << "U " << post["username"].asString() << "\n";
                            followingStream << "W " << post["message"].asString() << "\n";
                            followingStream << "\n";
                        }

                        sem_post(fileSem);
                        sem_close(fileSem);
                    }
                }
            }
        }
    }

private:
    void updateAllUsersFile(const std::vector<std::string> &users)
    {
        // had to sepaate this function as they use the same semaphore
        std::string usersFile = "./cluster_" + std::to_string(clusterID) + "/" + clusterSubdirectory + "/all_users.txt";
        std::string semName = "/" + std::to_string(clusterID) + "_" + clusterSubdirectory + "_all_users.txt";
        sem_t *fileSem = sem_open(semName.c_str(), O_CREAT, 0666, 1);

        sem_wait(fileSem);
        std::vector<std::string> newUsers;
        for (std::string user : users)
        {
            if (!file_contains_user(usersFile, user))
            {
                newUsers.push_back(user);
            }
        }

        std::ofstream userStream(usersFile, std::ios::app | std::ios::out | std::ios::in);
        for (const auto &user : newUsers)
        {
            userStream << user << std::endl;
        }
        userStream.close();
        sem_post(fileSem);
        sem_close(fileSem);
    }
};

void run_synchronizer(std::string coordIP, std::string coordPort, std::string port, int synchID, SynchronizerRabbitMQ &rabbitMQ);

class SynchServiceImpl final : public SynchService::Service
{
    // You do not need to modify this in any way
};

void RunServer(std::string coordIP, std::string coordPort, std::string port_no, int synchID)
{
    // localhost = 127.0.0.1
    std::string server_address("127.0.0.1:" + port_no);
    log(INFO, "Starting synchronizer server at " + server_address);
    SynchServiceImpl service;
    // grpc::EnableDefaultHealthCheckService(true);
    // grpc::reflection::InitProtoReflectionServerBuilderPlugin();
    ServerBuilder builder;
    // Listen on the given address without any authentication mechanism.
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    // Register "service" as the instance through which we'll communicate with
    // clients. In this case it corresponds to an *synchronous* service.
    builder.RegisterService(&service);
    // Finally assemble the server.
    std::unique_ptr<Server> server(builder.BuildAndStart());
    std::cout << "Server listening on " << server_address << std::endl;

    // Initialize RabbitMQ connection
    // SynchronizerRabbitMQ rabbitMQ("localhost", 5672, synchID);
    SynchronizerRabbitMQ rabbitMQ("rabbitmq", 5672, synchID);

    std::thread t1(run_synchronizer, coordIP, coordPort, port_no, synchID, std::ref(rabbitMQ));

    // Create a consumer thread
    std::thread consumerThread([&rabbitMQ]()
                               {
        while (true) {
            std::pair<std::string, std::string> msg_pair = rabbitMQ.consumeMessage(1000);

            if (!msg_pair.first.empty()) {
                const std::string& message = msg_pair.first;
                const std::string& routingKey = msg_pair.second;

                log(INFO, "Consumed message with routing key: " + routingKey);

                if (routingKey.find("users") != std::string::npos) {
                    rabbitMQ.consumeUserLists(message);
                } else if (routingKey.find("relations") != std::string::npos) {
                    rabbitMQ.consumeClientRelations(message);
                } else if (routingKey.find("timeline") != std::string::npos) {
                    rabbitMQ.consumeTimelines(message);
                } else {
                    log(WARNING, "Received message with unknown routing key: " + routingKey);
                }
            }
            std::this_thread::sleep_for(std::chrono::seconds(5));
            // you can modify this sleep period as per your choice
        } });

    server->Wait();

    //   t1.join();
    //   consumerThread.join();
}

int main(int argc, char **argv)
{
    int opt = 0;
    std::string coordIP;
    std::string coordPort;
    std::string port = "3029";

    while ((opt = getopt(argc, argv, "h:k:p:i:")) != -1)
    {
        switch (opt)
        {
        case 'h':
            coordIP = optarg;
            break;
        case 'k':
            coordPort = optarg;
            break;
        case 'p':
            port = optarg;
            break;
        case 'i':
            synchID = std::stoi(optarg);
            break;
        default:
            std::cerr << "Invalid Command Line Argument\n";
        }
    }

    std::string log_file_name = std::string("synchronizer-") + port;
    google::InitGoogleLogging(log_file_name.c_str());
    log(INFO, "Logging Initialized. Synchronizer starting...");

    coordAddr = coordIP + ":" + coordPort;
    clusterID = ((synchID - 1) % 3) + 1;
    ServerInfo serverInfo;
    serverInfo.set_hostname("localhost");
    serverInfo.set_port(port);
    serverInfo.set_type("synchronizer");
    serverInfo.set_serverid(synchID);
    serverInfo.set_clusterid(clusterID);
    Heartbeat(coordIP, coordPort, serverInfo, synchID);

    std::string target_str = coordIP + ":" + coordPort;
    log(INFO, "Connecting to coordinator at: " + target_str);
    log(INFO, "Creating gRPC channel to coordinator");
    coordinator_stub_ = std::unique_ptr<CoordService::Stub>(CoordService::NewStub(grpc::CreateChannel(target_str, grpc::InsecureChannelCredentials())));
    if (!coordinator_stub_)
    {
        log(ERROR, "Failed to create gRPC channel to coordinator: " + target_str);
    }
    RunServer(coordIP, coordPort, port, synchID);

    return 0;
}

void run_synchronizer(std::string coordIP, std::string coordPort, std::string port, int synchID, SynchronizerRabbitMQ &rabbitMQ)
{
    log(INFO, "Starting synchronizer with ID: " + std::to_string(synchID));
    // setup coordinator stub

    ServerInfo msg;
    Confirmation c;

    msg.set_serverid(synchID);
    msg.set_hostname("127.0.0.1");
    msg.set_port(port);
    msg.set_type("follower");

    // Make directory for cluster
    log(INFO, "Creating directories for cluster " + std::to_string(clusterID));
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

    // TODO: begin synchronization process
    while (true)
    {
        // the synchronizers sync files every 5 seconds
        sleep(5);

        grpc::ClientContext context;
        ServerList followerServers;
        ID id;
        id.set_id(synchID);

        // making a request to the coordinator to see count of follower synchronizers
        coordinator_stub_->GetAllFollowerServers(&context, id, &followerServers);

        std::vector<int> server_ids;
        std::vector<std::string> hosts, ports;
        for (std::string host : followerServers.hostname())
        {
            hosts.push_back(host);
        }
        for (std::string port : followerServers.port())
        {
            ports.push_back(port);
        }
        for (int serverid : followerServers.serverid())
        {
            server_ids.push_back(serverid);
        }

        // update the count of how many follower sychronizer processes the coordinator has registered
        total_number_of_registered_synchronizers = followerServers.serverid_size();

        // below here, you run all the update functions that synchronize the state across all the clusters
        // make any modifications as necessary to satisfy the assignments requirements
        clusterID = ((synchID - 1) % 3) + 1;
        ServerInfo serverInfo;
        serverInfo.set_hostname("localhost");
        serverInfo.set_port(port);
        serverInfo.set_type("synchronizer");
        serverInfo.set_serverid(synchID);
        serverInfo.set_clusterid(clusterID);
        Heartbeat(coordIP, coordPort, serverInfo, synchID);

        if (isMaster)
        {
            // Publish user list
            rabbitMQ.publishUserList();

            // Publish client relations
            rabbitMQ.publishClientRelations();

            // Publish timelines
            rabbitMQ.publishTimelines();
        }
    }
    return;
}

std::vector<std::string> get_lines_from_file(std::string filename)
{
    std::vector<std::string> users;
    std::string user;
    std::ifstream file(filename); // Open the file directly in the constructor

    if (!file.is_open()) {
        std::cerr << "Error opening file: " << filename << std::endl;
        return users; // Return empty vector if file can't be opened
    }

    while (getline(file, user))
    {
        if (!user.empty()) { // Only add non-empty lines
            users.push_back(user);
        }
    }

    file.close();
    return users;
}

void Heartbeat(std::string coordinatorIp, std::string coordinatorPort, ServerInfo serverInfo, int syncID)
{
    // For the synchronizer, a single initial heartbeat RPC acts as an initialization method which
    // servers to register the synchronizer with the coordinator and determine whether it is a master

    log(INFO, "Sending initial heartbeat to coordinator");
    std::string coordinatorInfo = coordinatorIp + ":" + coordinatorPort;
    std::unique_ptr<CoordService::Stub> stub = std::unique_ptr<CoordService::Stub>(CoordService::NewStub(grpc::CreateChannel(coordinatorInfo, grpc::InsecureChannelCredentials())));

    // send a heartbeat to the coordinator, which registers your follower synchronizer as either a master or a slave

    // YOUR CODE HERE
    grpc::ClientContext context;
    csce438::Confirmation reply;

    grpc::Status status = stub->Heartbeat(&context, serverInfo, &reply);
    if (status.ok())
    {
        log(INFO, "Synchronizer " + std::to_string(serverInfo.serverid()) + " Heartbeat sent successfully");
        if (reply.ismaster())
        {
            log(INFO, "Synchronizer " + std::to_string(serverInfo.serverid()) + " is a master");
            isMaster = true;
            clusterSubdirectory = "1";
        }
        else
        {
            log(INFO, "Synchronizer " + std::to_string(serverInfo.serverid()) + " is a slave");
            isMaster = false;
            clusterSubdirectory = "2";
        }
    }
    else
    {
        log(ERROR, "Failed to send heartbeat: " + status.error_message());
    }
}

bool file_contains_user(std::string filename, std::string user)
{
    std::vector<std::string> users;
    // check username is valid

    users = get_lines_from_file(filename);
    for (int i = 0; i < users.size(); i++)
    {
        std::cout<<"Checking if "<<user<<" = "<<users[i]<<std::endl;
        if (user == users[i])
        {
            std::cout<<"found"<<std::endl;
            return true;
        }
    }
    std::cout<<"not found"<<std::endl;
    return false;
}

std::vector<std::string> get_all_users_func(int synchID)
{
    // read all_users file master and client for correct serverID
    // std::string master_users_file = "./master"+std::to_string(synchID)+"/all_users";
    // std::string slave_users_file = "./slave"+std::to_string(synchID)+"/all_users";
    std::string clusterID = std::to_string(((synchID - 1) % 3) + 1);
    std::string master_users_file = "./cluster_" + clusterID + "/1/all_users.txt";
    std::string slave_users_file = "./cluster_" + clusterID + "/2/all_users.txt";
    // take longest list and package into AllUsers message

    std::string semNameMaster = "/" + clusterID + "_1_all_users.txt";
    sem_t *fileSemMaster = sem_open(semNameMaster.c_str(), O_CREAT, 0666, 1);
    sem_wait(fileSemMaster);
    std::vector<std::string> master_user_list = get_lines_from_file(master_users_file);
    sem_post(fileSemMaster);
    sem_close(fileSemMaster);

    std::string semNameSlave = "/" + clusterID + "_2_all_users.txt";
    sem_t *fileSemSlave = sem_open(semNameSlave.c_str(), O_CREAT, 0666, 1);
    sem_wait(fileSemSlave);
    std::vector<std::string> slave_user_list = get_lines_from_file(slave_users_file);
    sem_post(fileSemSlave);
    sem_close(fileSemSlave);

    if (master_user_list.size() >= slave_user_list.size())
        return master_user_list;
    else
        return slave_user_list;
}

std::vector<std::string> get_tl_or_fl(int synchID, int clientID, bool tl)
{
    // std::string master_fn = "./master"+std::to_string(synchID)+"/"+std::to_string(clientID);
    // std::string slave_fn = "./slave"+std::to_string(synchID)+"/" + std::to_string(clientID);
    std::string master_fn = "cluster_" + std::to_string(clusterID) + "/1/" + std::to_string(clientID);
    std::string slave_fn = "cluster_" + std::to_string(clusterID) + "/2/" + std::to_string(clientID);
    if (tl)
    {
        master_fn.append("_timeline.txt");
        slave_fn.append("_timeline.txt");
    }
    else
    {
        master_fn.append("_followers.txt");
        slave_fn.append("_followers.txt");
    }

    std::string semNameMaster = "/" + std::to_string(clusterID) + "_1_" + master_fn.substr(master_fn.find_last_of("/") + 1);
    sem_t *fileSemMaster = sem_open(semNameMaster.c_str(), O_CREAT, 0666, 1);
    sem_wait(fileSemMaster);
    std::vector<std::string> m = get_lines_from_file(master_fn);
    sem_post(fileSemMaster);
    sem_close(fileSemMaster);

    std::string semNameSlave = "/" + std::to_string(clusterID) + "_2_" + slave_fn.substr(slave_fn.find_last_of("/") + 1);
    sem_t *fileSemSlave = sem_open(semNameSlave.c_str(), O_CREAT, 0666, 1);
    sem_wait(fileSemSlave);
    std::vector<std::string> s = get_lines_from_file(slave_fn);
    sem_post(fileSemSlave);
    sem_close(fileSemSlave);

    if (m.size() >= s.size())
    {
        return m;
    }
    else
    {
        return s;
    }
}

std::vector<std::string> getFollowersOfUser(int ID)
{
    std::vector<std::string> followers;
    std::string clientID = std::to_string(ID);
    std::vector<std::string> usersInCluster = get_all_users_func(synchID);

    for (auto userID : usersInCluster)
    { // Examine each user's following file
        std::string file = "cluster_" + std::to_string(clusterID) + "/" + clusterSubdirectory + "/" + userID + "_follow_list.txt";
        std::string semName = "/" + std::to_string(clusterID) + "_" + clusterSubdirectory + "_" + userID + "_follow_list.txt";
        sem_t *fileSem = sem_open(semName.c_str(), O_CREAT, 0666, 1);

        sem_wait(fileSem);
        // std::cout << "Reading file " << file << std::endl;
        if (file_contains_user(file, clientID))
        {
            followers.push_back(userID);
        }
        sem_post(fileSem);
        sem_close(fileSem);
    }

    return followers;
}
