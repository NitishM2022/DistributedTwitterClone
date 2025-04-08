#include <algorithm>
#include <cstdio>
#include <ctime>

#include <google/protobuf/timestamp.pb.h>
#include <google/protobuf/duration.pb.h>
#include <chrono>
#include <sys/stat.h>
#include <sys/types.h>
#include <utility>
#include <vector>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <mutex>
#include <stdlib.h>
#include <unistd.h>
#include <google/protobuf/util/time_util.h>
#include <grpc++/grpc++.h>
#include <glog/logging.h>
#define log(severity, msg) \
    LOG(severity) << msg;  \
    google::FlushLogFiles(google::severity);

#include "coordinator.grpc.pb.h"
#include "coordinator.pb.h"

using csce438::Confirmation;
using csce438::CoordService;
using csce438::ID;
using csce438::ServerInfo;
using csce438::ServerList;
using csce438::SynchService;
using google::protobuf::Duration;
using google::protobuf::Timestamp;
using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerReader;
using grpc::ServerReaderWriter;
using grpc::ServerWriter;
using grpc::Status;

struct zNode
{
    int serverID;
    std::string hostname;
    std::string port;
    std::string type;
    std::time_t last_heartbeat;
    bool missed_heartbeat;
    int clusterID;
    bool isMaster;

    // function to check if the node is active
    bool isActive();
};

// potentially thread safe
std::mutex v_mutex;
std::vector<zNode *> cluster1;
std::vector<zNode *> cluster2;
std::vector<zNode *> cluster3;

// creating a vector of vectors containing znodes
std::vector<std::vector<zNode *>> clusters = {cluster1, cluster2, cluster3};

std::atomic<bool> alive = true;

// func declarations
int findServer(std::vector<zNode *> v, int id, std::string type);
int findServerByAddr(std::vector<zNode *> v, std::string hostname, std::string port, std::string type);
bool findMaster(std::vector<zNode *> v, std::string type);
std::time_t getTimeNow();
void checkHeartbeat();

bool zNode::isActive()
{
    bool status = false;
    if (!missed_heartbeat)
    {
        status = true;
    }
    else if (difftime(getTimeNow(), last_heartbeat) < 10)
    {
        status = true;
    }
    return status;
}

class CoordServiceImpl final : public CoordService::Service
{

    Status Heartbeat(ServerContext *context, const ServerInfo *serverinfo, Confirmation *confirmation) override
    {
        int serverID = serverinfo->serverid();
        std::string hostname = serverinfo->hostname();
        std::string port = serverinfo->port();
        std::string type = serverinfo->type();
        int clusterID = serverinfo->clusterid();

        if (type == "synchronizer")
        {
            v_mutex.lock();
            int index = findServer(clusters[clusterID - 1], serverID, type);
            if (index != -1)
            {
                log(INFO, "Heartbeat received from synchronizer " + std::to_string(serverID));
                clusters[clusterID - 1][index]->last_heartbeat = getTimeNow();
                clusters[clusterID - 1][index]->missed_heartbeat = false;
                bool masterPresent = findMaster(clusters[clusterID - 1], type);
                if (!masterPresent){
                    clusters[clusterID - 1][index]->isMaster = true;
                    log(INFO, "Synchronizer " + std::to_string(serverID) + " is now a master");
                }
                confirmation->set_ismaster(clusters[clusterID - 1][index]->isMaster);
            }
            else
            {
                log(INFO, "Heartbeat received from new synchronizer " + std::to_string(serverID));
                // When a new synchronizer is added, check if there is already a master in the cluster
                bool masterPresent = findMaster(clusters[clusterID - 1], type);
                confirmation->set_ismaster(!masterPresent);
                clusters[clusterID - 1].push_back(new zNode{serverID, hostname, port, type, getTimeNow(), false, clusterID, !masterPresent});
            }
            v_mutex.unlock();
        }
        else if (type == "server")
        {
            auto metadata = context->client_metadata();
            auto clusterIDIter = metadata.find("clusterid");

            v_mutex.lock();
            if (clusterIDIter != metadata.end() && findServerByAddr(clusters[clusterID - 1], hostname, port, type) == -1) // already registered
            {
                log(INFO, "Heartbeat received from new server " + std::to_string(serverID));
                // When a new server is added, check if there is already a master in the cluster
                int clusterID = std::stoi(std::string(clusterIDIter->second.data(), clusterIDIter->second.size()));
                bool masterPresent = findMaster(clusters[clusterID - 1], type);
                confirmation->set_ismaster(!masterPresent);
                clusters[clusterID - 1].push_back(new zNode{serverID, hostname, port, type, getTimeNow(), false, clusterID, !masterPresent});
            }
            else
            {
                int index = findServerByAddr(clusters[clusterID - 1], hostname, port, type);
                if (index != -1)
                {
                    log(INFO, "Heartbeat received from server " + std::to_string(serverID));
                    clusters[clusterID - 1][index]->last_heartbeat = getTimeNow();
                    clusters[clusterID - 1][index]->missed_heartbeat = false;
                    bool masterPresent = findMaster(clusters[clusterID - 1], type);
                    if (!masterPresent){
                        clusters[clusterID - 1][index]->isMaster = true;
                        log(INFO, "Server " + std::to_string(serverID) + " is now a master");
                    }
                    confirmation->set_ismaster(clusters[clusterID - 1][index]->isMaster);
                }
            }
            v_mutex.unlock();
        }

        return Status::OK;
    }

    // function returns the server information for requested client id
    // this function assumes there are always 3 clusters and has math
    // hardcoded to represent this.
    Status GetServer(ServerContext *context, const ID *id, ServerInfo *serverinfo) override
    {
        int clientID = id->id();
        int clusterID = ((clientID - 1) % 3) + 1;
        int serverId = 0;

        log(INFO, "GetServer called by client " + std::to_string(clientID) + " for cluster " + std::to_string(clusterID));

        v_mutex.lock();
        log(INFO, "Mutex unlocked " + std::to_string(clientID) + " for cluster " + std::to_string(clusterID));
        if (clusterID >= 1 && clusterID <= clusters.size() && !clusters[clusterID - 1].empty()) {
            for (const auto& server_node : clusters[clusterID - 1]) {
                // Client only talks to master servers & master servers are always active
                log(INFO, "Checking server " + std::to_string(server_node->serverID) + " for client " + std::to_string(clientID));
                if (server_node->type == "server" && server_node->isMaster) {
                    log(INFO, "Found active master server for client " + std::to_string(clientID) + " in cluster " + std::to_string(clusterID));
                    serverinfo->set_serverid(server_node->serverID);
                    serverinfo->set_hostname(server_node->hostname);
                    serverinfo->set_port(server_node->port);
                    serverinfo->set_type(server_node->type);
                    serverinfo->set_clusterid(server_node->clusterID);
                    serverinfo->set_ismaster(server_node->isMaster);
                    v_mutex.unlock();
                    return Status::OK;
                }
            }
            log(WARNING, "No active master server found in cluster " + std::to_string(clusterID) + " for client " + std::to_string(clientID));
            v_mutex.unlock();
            return Status::OK;
        } 

        log(ERROR, "Invalid cluster ID: " + std::to_string(clusterID) + " for client " + std::to_string(clientID));
        v_mutex.unlock();
        return Status::OK;
    }

    Status GetAllFollowerServers(ServerContext *context, const ID *id, ServerList *serverlist) override
    {

        v_mutex.lock();

        log(INFO, "GetAllFollowerServers called by synchronizer");
        for (auto &cluster : clusters)
        {
            for (auto &server : cluster)
            {
                if (server->type == "synchronizer" && server->isActive())
                {
                    serverlist->add_serverid(server->serverID);
                    serverlist->add_hostname(server->hostname);
                    serverlist->add_port(server->port);
                    serverlist->add_type(server->type);
                    serverlist->add_clusterid(std::to_string(server->clusterID).c_str());
                    serverlist->add_ismaster(server->isMaster);
                }
            }
        }

        v_mutex.unlock();

        return Status::OK;
    }
};

void RunServer(std::string port_no)
{
    // start thread to check heartbeats
    std::thread hb(checkHeartbeat);
    // localhost = 127.0.0.1
    std::string server_address("127.0.0.1:" + port_no);
    CoordServiceImpl service;
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

    // Wait for the server to shutdown. Note that some other thread must be
    // responsible for shutting down the server for this call to ever return.
    server->Wait();
    alive.store(false);
}

int main(int argc, char **argv)
{

    std::string port = "3010";
    int opt = 0;
    while ((opt = getopt(argc, argv, "p:")) != -1)
    {
        switch (opt)
        {
        case 'p':
            port = optarg;
            break;
        default:
            std::cerr << "Invalid Command Line Argument\n";
        }
    }

    std::string log_file_name = std::string("coordinator-") + port;
    google::InitGoogleLogging(log_file_name.c_str());
    log(INFO, "Logging Initialized. Coordinator starting...");
    RunServer(port);
    return 0;
}

void checkHeartbeat()
{
    while (alive.load())
    {
        // check servers for heartbeat > 10
        // if true turn missed heartbeat = true
        //  Your code below

        v_mutex.lock();

        // iterating through the clusters vector of vectors of znodes
        for (auto &c : clusters)
        {
            for (auto &s : c)
            {
                if (difftime(getTimeNow(), s->last_heartbeat) > 10)
                {
                    log(WARNING, "Missed heartbeat from " + s->type + " " + std::to_string(s->serverID));
                    s->missed_heartbeat = true;
                    s->isMaster = false; // Demote if heartbeat is missed
                }
            }
        }

        v_mutex.unlock();

        sleep(3);
    }
}

std::time_t getTimeNow()
{
    return std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
}

int findServer(std::vector<zNode *> c, int id, std::string type)
{
    for (int i = 0; i < c.size(); i++)
    {
        zNode *z = c[i];
        if (z->serverID == id && z->type == type)
        {
            return i;
        }
    }
    return -1;
}

int findServerByAddr(std::vector<zNode *> c, std::string hostname, std::string port, std::string type)
{
    for (int i = 0; i < c.size(); i++)
    {
        zNode *z = c[i];
        if (z->hostname == hostname && z->port == port && z->type == type)
        {
            return i;
        }
    }
    return -1;
}

bool findMaster(std::vector<zNode *> c, std::string type)
{
    for (auto &z : c)
    {
        if (z->type == type && z->isMaster)
        {
            return true;
        }
    }
    return false;
}
