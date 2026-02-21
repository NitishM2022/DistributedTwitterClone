# Distributed Twitter Clone

This document describes the comprehensive architecture and system design of the distributed, fault-tolerant Twitter clone. The system leverages RPC for client-server communication, a central coordinator for cluster management, and RabbitMQ message queues for cross-cluster synchronization.

## 1) What runs in the system

- **1 Coordinator process** (`coordinator`)
- **6 Server replicas** (`tsd`), exactly 2 per cluster.
- **6 Synchronizer replicas** (`synchronizer`), exactly 2 per cluster.
- **1 RabbitMQ broker** (message bus)
- **Clients** (`tsc`) that connect to the coordinator to locate their respective cluster server.

User-to-cluster mapping is fixed via hashing:
`cluster = ((user_id - 1) % 3) + 1`

## 2) Build and Run

### Build Setup

```bash
cd mp2_2
# Assuming docker environment setup
sudo apt-get install docker-compose -y
docker-compose up -d

# Enable RabbitMQ plugins
docker exec rabbitmq_container rabbitmq-plugins enable rabbitmq_stream rabbitmq_stream_management

# Access the container
docker exec -it csce438_mp2_2_container bash -c "cd /home/csce438/mp2_2 && exec /bin/bash"

# Initial setup
chmod +x setup.sh startup.sh stop.sh
./setup.sh

# Compile
make -j4
```

To clear the directory (and remove `.txt` DB files): `make clean`

### Running the System Manually

If not using `./startup.sh`:

- **Coordinator:** `./coordinator -p 9000` _(Add `GLOG_logtostderr=1` for logs)_
- **Server:** `./tsd -c <clusterId> -s <serverId> -h <coordinatorIP> -k <coordinatorPort> -p <portNum>`
- **Synchronizer:** `./synchronizer -h <coordinatorIP> -k <coordinatorPort> -p <portNum> -i <synchID>`

Or run everything automatically:

```bash
./startup.sh
```

---

## 3) Deployment map (Macro View)

```mermaid
flowchart TB
  Client["Clients (tsc)"]
  Coord["Coordinator<br/>(Manages Heartbeats & Leaders)"]
  Bus["RabbitMQ Message Bus<br/>(Cross-Cluster Pub/Sub)"]

  subgraph C1 ["Cluster 1"]
    S1M["Server Master (tsd)"]
    S1S["Server Slave (tsd)"]
    Y1M["Sync Master"]
    Y1S["Sync Slave"]
  end

  subgraph C2 ["Cluster 2"]
    S2M["Server Master (tsd)"]
    S2S["Server Slave (tsd)"]
    Y2M["Sync Master"]
    Y2S["Sync Slave"]
  end

  subgraph C3 ["Cluster 3"]
    S3M["Server Master (tsd)"]
    S3S["Server Slave (tsd)"]
    Y3M["Sync Master"]
    Y3S["Sync Slave"]
  end

  Client -->|rpc GetServer| Coord
  Client -->|rpc Timeline Follow| S1M
  S1M -->|rpc stream| Client

  Coord -.->|Heartbeat| S1M
  S1M -.->|Heartbeat| Coord
  Coord -.->|Heartbeat| S2M
  S2M -.->|Heartbeat| Coord
  Coord -.->|Heartbeat| S3M
  S3M -.->|Heartbeat| Coord

  Y1M --->|Publish| Bus
  Y2M --->|Publish| Bus
  Y3M --->|Publish| Bus

  Bus --->|Consume| Y1M
  Bus --->|Consume| Y1S
  Bus --->|Consume| Y2M
  Bus --->|Consume| Y2S
  Bus --->|Consume| Y3M
  Bus --->|Consume| Y3S
```

---

## 4) Intra-Cluster Architecture (Micro View)

Inside each cluster, processes are strictly divided into active **Masters** and standby **Slaves**. The Master Synchronizer is solely responsible for publishing to RabbitMQ. Both Master and Slave Synchronizers consume from RabbitMQ to keep their local file layouts synced. The Master Server forwards state-changing operations to the Slave Server.

```mermaid
flowchart LR
  U["Client"] -->|gRPC| SM["Server Master (tsd)"]
  SM -->|Forward Follow/Login| SF["Server Slave (tsd)"]

  subgraph Storage [Persistent Storage]
    DM["Master Store Directory<br/>(cluster_X/1)"]
    DF["Slave Store Directory<br/>(cluster_X/2)"]
  end

  SM <--> DM
  SF <--> DF

  SYM["Sync Master"] -->|Reads Files| DM
  SYM -->|Publishes JSON Lists| MQ["RabbitMQ Exchange"]

  MQ -->|Consumes| SYCM["Sync Master Consumer Thread"]
  MQ -->|Consumes| SYCS["Sync Slave Consumer Thread"]

  SYCM -->|Writes Updates| DM
  SYCS -->|Writes Updates| DF
```

---

## 5) Control Plane & Leader Election

The Coordinator handles dynamic assignment of `Master` and `Slave` roles using a 5-second Heartbeat mechanism.

### Heartbeat Sequence

```mermaid
sequenceDiagram
    participant S as Server/Sync Node
    participant C as Coordinator
    participant U as Client

    loop Every 5 seconds
        S->>C: Heartbeat(NodeID, ClusterID, Type)
        alt Cluster has no Master
            C-->>S: HeartbeatReply(isMaster = true)
        else Master exists
            C-->>S: HeartbeatReply(isMaster = false)
        end
    end

    Note over C: Background checkHeartbeat thread (every 3s)<br/>Demotes nodes if last_heartbeat > 10s

    U->>C: GetServer(user_id)
    C->>C: Hash UserID to find Target Cluster
    C-->>U: Return Active Server Master Endpoint
```

---

## 6) Cross-Cluster Synchronization (RabbitMQ)

The **Synchronizer** daemon maintains eventual consistency across clusters. Cross-cluster data is handled using three internal publish/consume RabbitMQ queues:

1. **UserList**
2. **ClientRelations (Followers)**
3. **Timelines (Posts)**

File access inside the Synchronizer is strictly protected using named semaphores (`sem_open`).

### Timeline Replication Sequence

When User 5 (Cluster 2) posts, and User 1 (Cluster 1) is a follower:

```mermaid
sequenceDiagram
    participant U5 as Client 5
    participant S2M as Cluster 2 Server Master
    participant Y2M as Cluster 2 Sync Master
    participant MQ as RabbitMQ
    participant Y1C as Cluster 1 Sync Consumer
    participant S1M as Cluster 1 Server Thread
    participant U1 as Client 1

    U5->>S2M: Write Post (Timeline Stream)
    S2M->>S2M: Append to cluster_2/1/u_timeline.txt

    loop Every 5s
        Y2M->>Y2M: Read all clients' timelines
        Y2M->>Y2M: Check u_followers.txt
        Y2M->>MQ: amqp_basic_publish(Timeline JSON object)
    end

    MQ-->>Y1C: Delivery via consumeMessage()
    Y1C->>Y1C: Hashmap checks delivered posts
    Y1C->>Y1C: Append to cluster_1/*/u_following.txt for User 1

    loop DB Thread
        S1M->>S1M: Detect new posts in u_following.txt
        S1M->>U1: Stream write newly detected posts
    end
```

### Queue Breakdown

#### 1. UserList Queue

- **Publisher (Sync Master):** Reads `all_users.txt` in its cluster directory. Compiles a JSON list of users and publishes it.
- **Consumer:** Reads JSON, iterates items, and appends missing users to the local `all_users.txt`.

#### 2. ClientRelations Queue

- **Publisher (Sync Master):** Reads `all_users.txt`. Iterates through all clients in its cluster. If a local client follows an external user, it compiles `{"User": ["Follower 1", "Follower 2"]}` and asks the Coordinator for target routing before publishing.
- **Consumer:** Reads JSON, finds external users and local followers, appending missing entries to `u_followers.txt`.

#### 3. Timelines Queue

- **Publisher (Sync Master):** Extracts unsynchronized posts for every local client. Checks `u_followers.txt` to find external followers. Contacts Coordinator to find target synchronization queues, then publishes JSON containing posts.
- **Consumer:** Maintains an in-memory hashmap tracking published post counts. Appends novel posts directly to the target follower's `u_following.txt` file.

---

## 7) Server Internal Architecture

The `tsd` server employs isolated threads to keep gRPC handlers unblocked:

```mermaid
stateDiagram-v2
    direction TB
    state "gRPC Handlers" as RPC {
        Login --> Follow
        Follow --> ForwardToSlave
    }

    state "DB Thread (Bidirectional Sync)" as DB {
        state "Read all_users.txt, u_follow_list.txt" as ReadFiles
        state "Update In-Memory `client_db`" as UpdateMem
        state "Persist `client_db` to Files" as WriteFiles
        ReadFiles --> UpdateMem
        UpdateMem --> WriteFiles
    }

    state "Timeline Threads" as Timeline {
        state "Write Thread" as Write
        state "Read Thread" as Read
    }
    Note right of Write: Monitors `u_following.txt` for new posts.<br/>Streams to client. Stops thread on `write()` fail.
    Note right of Read: Appends incoming posts to `u_timeline.txt`.<br/>Appends to `u_following.txt` of intra-cluster followers.
```

## 8) Run layout used by startup script

- **Coordinator:** `9000`
- **Servers:**
  - Cluster 1: `10000`, `10003`
  - Cluster 2: `10001`, `10004`
  - Cluster 3: `10002`, `10005`
- **Synchronizers:**
  - IDs `1..6`, ports `9001..9006`
